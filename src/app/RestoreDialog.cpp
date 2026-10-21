// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RestoreDialog.h"

#include "Session.h"

#include "ui_RestoreDialog.h"

#include <QFileDialog>
#include <QFileInfo>

namespace slonisko {

RestoreDialog::RestoreDialog(Session *session, const QString &database, QSettings *settings,
                             QWidget *parent)
    : ToolDialog(session, database, settings, parent), m_ui(std::make_unique<Ui::RestoreDialog>())
{
    m_ui->setupUi(this);
    setWidgets({m_ui->filePage, m_ui->tool, m_ui->chooseTool, m_ui->problem, m_ui->runPage,
                m_ui->command, m_ui->log, m_ui->busy, m_ui->status},
               tr("&Restore"));
    m_ui->databaseName->setText(databaseName());
    fillContents(m_ui->content);
    m_ui->exitOnError->setChecked(true);

    connect(m_ui->browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Restore"), m_ui->path->text(),
            tr("Backups (*.dump *.backup *.sql *.tar);;All files (*)"));
        if (!path.isEmpty())
            setPath(path);
    });
    connect(m_ui->browseDirectory, &QPushButton::clicked, this, [this] {
        const QString path
            = QFileDialog::getExistingDirectory(this, tr("Restore"), m_ui->path->text());
        if (!path.isEmpty())
            setPath(path);
    });
    connect(m_ui->path, &QLineEdit::textChanged, this, &RestoreDialog::pathChanged);
    for (QLineEdit *edit : {m_ui->schemas, m_ui->tables})
        connect(edit, &QLineEdit::textChanged, this, &RestoreDialog::update);
    connect(m_ui->content, &QComboBox::currentIndexChanged, this, &RestoreDialog::update);
    for (QCheckBox *box : {m_ui->clean, m_ui->create, m_ui->noOwner, m_ui->noPrivileges,
                           m_ui->singleTransaction, m_ui->exitOnError})
        connect(box, &QCheckBox::toggled, this, &RestoreDialog::pathChanged);
    connect(m_ui->jobs, &QSpinBox::valueChanged, this, &RestoreDialog::update);
    pathChanged();
}

RestoreDialog::~RestoreDialog() = default;

void RestoreDialog::setPath(const QString &path)
{
    m_ui->path->setText(path);
}

bool RestoreDialog::isScript() const
{
    return pg::archiveKind(m_ui->path->text().trimmed()) == pg::ArchiveKind::Plain;
}

pg::RestoreOptions RestoreDialog::options() const
{
    pg::RestoreOptions options;
    options.path = m_ui->path->text().trimmed();
    options.singleTransaction = m_ui->singleTransaction->isChecked();
    options.exitOnError = m_ui->exitOnError->isChecked();
    if (isScript())
        return options; // A script does what it says; psql has no say in it.
    options.content = pg::DumpContent(m_ui->content->currentData().toInt());
    options.schemas = nameList(m_ui->schemas->text());
    options.tables = nameList(m_ui->tables->text());
    options.clean = m_ui->clean->isChecked();
    options.create = m_ui->create->isChecked();
    options.noOwner = m_ui->noOwner->isChecked();
    options.noPrivileges = m_ui->noPrivileges->isChecked();
    options.jobs = options.singleTransaction ? 1 : m_ui->jobs->value();
    return options;
}

int RestoreDialog::nextId() const
{
    // A script has no contents to choose from.
    if (currentPage() == m_ui->filePage && isScript()) {
        for (const int id : pageIds()) {
            if (page(id) == m_ui->optionsPage)
                return id;
        }
    }
    return ToolDialog::nextId();
}

QString RestoreDialog::programName() const
{
    return isScript() ? QStringLiteral("psql") : QStringLiteral("pg_restore");
}

QStringList RestoreDialog::arguments(const QByteArray &conninfo) const
{
    return isScript() ? pg::psqlArguments(options(), conninfo)
                      : pg::restoreArguments(options(), conninfo);
}

QString RestoreDialog::problem() const
{
    if (m_ui->path->text().trimmed().isEmpty())
        return tr("Choose the backup to restore.");
    if (pg::archiveKind(m_ui->path->text().trimmed()) == pg::ArchiveKind::Missing)
        return tr("That is no file, nor a directory pg_dump made.");
    return {};
}

QString RestoreDialog::question() const
{
    QString text = tr("Restore %1 into the database %2 of %3?")
                       .arg(QFileInfo(options().path).fileName(), databaseName(),
                            session() ? session()->profile().displayName() : QString());
    if (options().clean)
        text += QLatin1Char('\n') + tr("What the backup holds is dropped from the database first.");
    return text;
}

QString RestoreDialog::doneMessage() const
{
    return tr("Restored %1.").arg(QFileInfo(options().path).fileName());
}

void RestoreDialog::pathChanged()
{
    const pg::ArchiveKind kind = pg::archiveKind(m_ui->path->text().trimmed());
    m_ui->kind->setText(m_ui->path->text().trimmed().isEmpty() ? QString()
                                                               : pg::archiveKindName(kind));
    // A script is run as it is; only an archive can be picked from.
    const bool archive = kind != pg::ArchiveKind::Plain;
    for (QWidget *w : std::initializer_list<QWidget *> {m_ui->clean, m_ui->create, m_ui->noOwner,
                                                        m_ui->noPrivileges}) {
        w->setEnabled(archive);
    }
    const bool parallel = archive && !m_ui->singleTransaction->isChecked();
    m_ui->jobsLabel->setEnabled(parallel);
    m_ui->jobs->setEnabled(parallel);
    update();
}

} // namespace slonisko
