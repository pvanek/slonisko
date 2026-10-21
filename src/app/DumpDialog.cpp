// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DumpDialog.h"

#include "Session.h"

#include "ui_DumpDialog.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>

namespace slonisko {

namespace {

// Where a file name was safe to make from: database, schema and table names
// may hold anything.
QString fileName(const QString &name)
{
    QString out = name;
    for (QChar &c : out) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            c = QLatin1Char('_');
    }
    return out;
}

} // namespace

// DumpDialog.

DumpDialog::DumpDialog(Session *session, const QString &database, const QString &schema,
                       const QString &table, QSettings *settings, QWidget *parent)
    : ToolDialog(session, database, settings, parent), m_ui(std::make_unique<Ui::DumpDialog>())
{
    m_ui->setupUi(this);
    setWidgets({m_ui->filePage, m_ui->tool, m_ui->chooseTool, m_ui->problem, m_ui->runPage,
                m_ui->command, m_ui->log, m_ui->busy, m_ui->status},
               tr("&Back Up"));
    m_ui->databaseName->setText(databaseName());
    for (const pg::DumpFormat format : {pg::DumpFormat::Custom, pg::DumpFormat::Plain,
                                        pg::DumpFormat::Directory, pg::DumpFormat::Tar})
        m_ui->format->addItem(pg::dumpFormatName(format), int(format));
    fillContents(m_ui->content);
    m_ui->schemas->setText(schema);
    if (!table.isEmpty())
        m_ui->tables->setText(schema.isEmpty() ? table : schema + QLatin1Char('.') + table);

    QStringList parts {databaseName()};
    if (!table.isEmpty())
        parts << table;
    else if (!schema.isEmpty())
        parts << schema;
    parts << QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    m_ui->path->setText(QDir(dir).filePath(fileName(parts.join(QLatin1Char('-')))
                                           + QStringLiteral(".")
                                           + pg::dumpFormatSuffix(pg::DumpFormat::Custom)));

    connect(m_ui->format, &QComboBox::currentIndexChanged, this, &DumpDialog::formatChanged);
    connect(m_ui->browse, &QPushButton::clicked, this, &DumpDialog::browse);
    for (QLineEdit *edit : {m_ui->path, m_ui->schemas, m_ui->tables})
        connect(edit, &QLineEdit::textChanged, this, &DumpDialog::update);
    connect(m_ui->content, &QComboBox::currentIndexChanged, this, &DumpDialog::update);
    for (QCheckBox *box :
         {m_ui->clean, m_ui->create, m_ui->noOwner, m_ui->noPrivileges, m_ui->inserts})
        connect(box, &QCheckBox::toggled, this, &DumpDialog::update);
    connect(m_ui->jobs, &QSpinBox::valueChanged, this, &DumpDialog::update);
    connect(m_ui->compression, &QSpinBox::valueChanged, this, &DumpDialog::update);
    formatChanged();
}

DumpDialog::~DumpDialog() = default;

pg::DumpOptions DumpDialog::options() const
{
    pg::DumpOptions options;
    options.format = pg::DumpFormat(m_ui->format->currentData().toInt());
    options.path = m_ui->path->text().trimmed();
    options.content = pg::DumpContent(m_ui->content->currentData().toInt());
    options.schemas = nameList(m_ui->schemas->text());
    options.tables = nameList(m_ui->tables->text());
    const bool script = options.format == pg::DumpFormat::Plain;
    options.clean = script && m_ui->clean->isChecked();
    options.create = script && m_ui->create->isChecked();
    options.noOwner = m_ui->noOwner->isChecked();
    options.noPrivileges = m_ui->noPrivileges->isChecked();
    options.inserts = m_ui->inserts->isChecked();
    options.jobs = options.format == pg::DumpFormat::Directory ? m_ui->jobs->value() : 1;
    options.compression = m_ui->compression->value();
    return options;
}

QString DumpDialog::programName() const
{
    return QStringLiteral("pg_dump");
}

QStringList DumpDialog::arguments(const QByteArray &conninfo) const
{
    return pg::dumpArguments(options(), conninfo);
}

QString DumpDialog::problem() const
{
    const pg::DumpOptions chosen = options();
    if (chosen.path.isEmpty())
        return tr("Choose where to save the backup.");
    const QFileInfo info(chosen.path);
    if (chosen.format == pg::DumpFormat::Directory) {
        if (info.exists() && (!info.isDir() || !QDir(chosen.path).isEmpty()))
            return tr("A directory backup goes into a new or empty directory.");
    } else if (info.isDir()) {
        return tr("%1 is a directory.").arg(chosen.path);
    }
    return {};
}

QString DumpDialog::question() const
{
    const pg::DumpOptions chosen = options();
    if (chosen.format != pg::DumpFormat::Directory && QFileInfo(chosen.path).isFile())
        return tr("%1 already exists. Replace it?").arg(chosen.path);
    return {};
}

QString DumpDialog::doneMessage() const
{
    return tr("Backed up to %1.").arg(QDir::toNativeSeparators(options().path));
}

void DumpDialog::formatChanged()
{
    const auto format = pg::DumpFormat(m_ui->format->currentData().toInt());
    const bool script = format == pg::DumpFormat::Plain;
    // An archive leaves these to the restore.
    m_ui->clean->setEnabled(script);
    m_ui->create->setEnabled(script);
    m_ui->jobsLabel->setEnabled(format == pg::DumpFormat::Directory);
    m_ui->jobs->setEnabled(format == pg::DumpFormat::Directory);
    m_ui->compressionLabel->setEnabled(format != pg::DumpFormat::Tar);
    m_ui->compression->setEnabled(format != pg::DumpFormat::Tar);

    // The suffix follows the format, unless the name is the user's own.
    const QString path = m_ui->path->text();
    const QFileInfo info(path);
    static const QStringList ours {QStringLiteral("dump"), QStringLiteral("sql"),
                                   QStringLiteral("tar")};
    QString base = path;
    if (ours.contains(info.suffix()))
        base.chop(info.suffix().size() + 1);
    if (const QString suffix = pg::dumpFormatSuffix(format); !suffix.isEmpty())
        base += QLatin1Char('.') + suffix;
    if (base != path && (ours.contains(info.suffix()) || info.suffix().isEmpty()))
        m_ui->path->setText(base);
    update();
}

void DumpDialog::browse()
{
    const auto format = pg::DumpFormat(m_ui->format->currentData().toInt());
    const QString suffix = pg::dumpFormatSuffix(format);
    const QString filter = suffix.isEmpty()
        ? QString()
        : QStringLiteral("%1 (*.%2)").arg(pg::dumpFormatName(format), suffix);
    // A directory backup names a directory pg_dump makes, so it is saved
    // like a file.
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Back Up To"), m_ui->path->text(), filter, nullptr,
        format == pg::DumpFormat::Directory ? QFileDialog::DontConfirmOverwrite
                                            : QFileDialog::Options());
    if (!path.isEmpty())
        m_ui->path->setText(path);
}

} // namespace slonisko
