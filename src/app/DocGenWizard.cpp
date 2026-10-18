// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DocGenWizard.h"

#include "Session.h"

#include "ui_DocGenWizard.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace slonisko {

DocGenWizard::DocGenWizard(Session *session, const QString &database, QWidget *parent, Kind kind)
    : QWizard(parent), m_ui(std::make_unique<Ui::DocGenWizard>()), m_kind(kind), m_session(session),
      m_database(database.isEmpty() && session ? session->profile().database : database),
      m_progressTimer(new QTimer(this))
{
    m_ui->setupUi(this);
    m_confirm = [this](const QString &question) {
        return QMessageBox::question(this, windowTitle(), question) == QMessageBox::Yes;
    };
    const bool report = kind == Kind::QualityReport;
    m_ui->documentationOptions->setVisible(!report);
    m_ui->reportOptions->setVisible(report);
    if (report) {
        setWindowTitle(tr("Database Quality Report"));
        m_ui->schemasPage->setSubTitle(tr("The schemas whose objects the report looks at."));
        m_ui->contentsPage->setSubTitle(tr("What the report says, and what it reads."));
        m_ui->generatePage->setSubTitle(tr("The catalog and the statistics are read and the "
                                           "report written."));
        m_ui->descriptionLabel->setText(tr("&Introduction:"));
        m_ui->description->setPlaceholderText(tr("Text for the summary page"));
        m_ui->title->setPlaceholderText(tr("\"<database> quality report\""));
    }
    if (session)
        setWindowTitle(
            (report ? tr("Quality Report of %1 (%2)") : tr("Schema Documentation of %1 (%2)"))
                .arg(m_database, session->profile().displayName()));

    m_ui->schemasPage->setComplete([this] {
        for (int i = 0; i < m_ui->schemas->count(); ++i) {
            if (m_ui->schemas->item(i)->checkState() == Qt::Checked)
                return true;
        }
        return false;
    });
    m_ui->outputPage->setComplete([this] { return !m_ui->output->text().trimmed().isEmpty(); });
    m_ui->contentsPage->setButtonText(QWizard::NextButton, tr("&Generate"));
    m_ui->generatePage->setComplete([this] { return m_succeeded && !m_running; });

    for (const docgen::Format format : docgen::allFormats())
        m_ui->format->addItem(docgen::formatName(format), int(format));

    const QSettings settings;
    const QString group = settingsGroup();
    m_ui->format->setCurrentIndex(std::max(
        0, m_ui->format->findData(settings.value(group + QStringLiteral("format")).toInt())));
    m_ui->output->setText(settings.value(group + QStringLiteral("directory")).toString());
    if (m_ui->output->text().isEmpty() && !m_database.isEmpty())
        m_ui->output->setText(QDir::home().filePath(
            m_database + (report ? QStringLiteral("-quality-report") : QStringLiteral("-schema"))));
    m_ui->antoraName->setText(
        settings
            .value(group + QStringLiteral("antoraName"),
                   report ? QStringLiteral("report") : QStringLiteral("schema"))
            .toString());
    for (QCheckBox *box : remembered())
        box->setChecked(settings.value(group + box->objectName(), box->isChecked()).toBool());

    connect(m_ui->browse, &QPushButton::clicked, this, &DocGenWizard::browse);
    connect(m_ui->format, &QComboBox::currentIndexChanged, this, &DocGenWizard::updateAntora);
    connect(m_ui->output, &QLineEdit::textChanged, m_ui->outputPage, &DocGenPage::updateComplete);
    connect(m_ui->schemas, &QListWidget::itemChanged, m_ui->schemasPage,
            &DocGenPage::updateComplete);
    const auto checkAll = [this](Qt::CheckState state) {
        for (int i = 0; i < m_ui->schemas->count(); ++i)
            m_ui->schemas->item(i)->setCheckState(state);
    };
    connect(m_ui->allSchemas, &QPushButton::clicked, this, [checkAll] { checkAll(Qt::Checked); });
    connect(m_ui->noSchemas, &QPushButton::clicked, this, [checkAll] { checkAll(Qt::Unchecked); });

    m_progressTimer->setInterval(100);
    connect(m_progressTimer, &QTimer::timeout, this, [this] {
        if (m_total && *m_total > 0) {
            m_ui->progress->setMaximum(*m_total);
            m_ui->progress->setValue(*m_done);
        }
    });

    updateAntora();
    loadSchemas();
}

DocGenWizard::~DocGenWizard() = default;

Session *DocGenWizard::session() const
{
    return m_session.data();
}

docgen::Options DocGenWizard::options() const
{
    docgen::Options options;
    QStringList checked;
    for (int i = 0; i < m_ui->schemas->count(); ++i) {
        if (m_ui->schemas->item(i)->checkState() == Qt::Checked)
            checked << m_ui->schemas->item(i)->text();
    }
    // All of them, schemas made after the list was read included.
    if (checked.size() < m_ui->schemas->count())
        options.schemas.include = checked;
    options.pages.title = m_ui->title->text().trimmed();
    options.pages.description = m_ui->description->toPlainText();
    options.pages.diagrams = m_ui->diagrams->isChecked();
    options.pages.definitions = m_ui->definitions->isChecked();
    options.pages.symbolIndex = m_ui->symbolIndex->isChecked();
    options.pages.partitions = m_ui->partitions->isChecked();
    options.output.format = docgen::Format(m_ui->format->currentData().toInt());
    options.output.directory = m_ui->output->text().trimmed();
    options.output.antoraName = m_ui->antoraName->text().trimmed();
    if (options.output.antoraName.isEmpty())
        options.output.antoraName = QStringLiteral("schema");
    return options;
}

quality::Options DocGenWizard::reportOptions() const
{
    const docgen::Options chosen = options();
    quality::Options options;
    options.schemas = chosen.schemas;
    options.report.title = chosen.pages.title;
    options.report.description = chosen.pages.description;
    options.output = chosen.output;
    options.output.antoraName = m_ui->antoraName->text().trimmed();
    if (options.output.antoraName.isEmpty())
        options.output.antoraName = QStringLiteral("report");
    options.health = m_ui->health->isChecked();
    options.notices = m_ui->notices->isChecked();
    return options;
}

QString DocGenWizard::settingsGroup() const
{
    return m_kind == Kind::QualityReport ? QStringLiteral("quality/") : QStringLiteral("docgen/");
}

QList<QCheckBox *> DocGenWizard::remembered() const
{
    if (m_kind == Kind::QualityReport)
        return {m_ui->health, m_ui->notices, m_ui->openWhenDone};
    return {m_ui->diagrams, m_ui->definitions, m_ui->symbolIndex, m_ui->partitions,
            m_ui->openWhenDone};
}

void DocGenWizard::loadSchemas()
{
    pg::QueryRunner *runner = m_session ? m_session->runner(m_database) : nullptr;
    if (!runner) {
        m_ui->schemasStatus->setText(tr("Not connected."));
        return;
    }
    m_ui->schemasStatus->setText(tr("Reading…"));
    runner->run(docgen::schemaListQuery(), this, [this](const pg::QueryOutcome &outcome) {
        if (!outcome.ok() || outcome.results.empty()) {
            m_ui->schemasStatus->setText(outcome.error.trimmed());
            return;
        }
        // Signals off, or every item added would count as checked by hand.
        const QSignalBlocker blocker(m_ui->schemas);
        for (const QString &name : docgen::parseSchemaList(outcome.results.back())) {
            auto *item = new QListWidgetItem(name, m_ui->schemas);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        const int count = m_ui->schemas->count();
        m_ui->schemasStatus->setText(count == 1 ? tr("1 schema") : tr("%1 schemas").arg(count));
        m_ui->schemasPage->updateComplete();
    });
}

void DocGenWizard::updateAntora()
{
    const bool antora
        = docgen::Format(m_ui->format->currentData().toInt()) == docgen::Format::Antora;
    m_ui->antoraLabel->setVisible(antora);
    m_ui->antoraName->setVisible(antora);
}

void DocGenWizard::browse()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        m_kind == Kind::QualityReport ? tr("Folder for the Report")
                                      : tr("Folder for the Documentation"),
        m_ui->output->text());
    if (!directory.isEmpty())
        m_ui->output->setText(QDir::toNativeSeparators(directory));
}

bool DocGenWizard::validateCurrentPage()
{
    if (currentPage() == m_ui->outputPage) {
        const QDir directory(m_ui->output->text().trimmed());
        if (directory.exists() && !directory.isEmpty()
            && !m_confirm(tr("%1 already has files in it. Files of the same names will be "
                             "replaced; the others stay. Write into it anyway?")
                              .arg(QDir::toNativeSeparators(directory.absolutePath()))))
            return false;
    }
    return QWizard::validateCurrentPage();
}

void DocGenWizard::initializePage(int id)
{
    QWizard::initializePage(id);
    // Each time it is reached, so that going back to fix what failed and
    // forward again tries again.
    if (page(id) == m_ui->generatePage)
        generate();
}

void DocGenWizard::setRunning(bool running)
{
    m_running = running;
    // No going back meanwhile. QWizard enables its buttons again itself
    // whenever a page changes or completes; this option it keeps to.
    setOption(QWizard::NoBackButtonOnLastPage, running);
    if (running) {
        // Busy until the pages are counted.
        m_ui->progress->setRange(0, 0);
        m_progressTimer->start();
    } else {
        m_progressTimer->stop();
    }
    m_ui->generatePage->updateComplete();
}

void DocGenWizard::generate()
{
    m_succeeded = false;
    m_result = {};
    pg::QueryRunner *runner = m_session ? m_session->runner(m_database) : nullptr;
    if (!runner) {
        fail(tr("Not connected."));
        return;
    }
    saveSettings();
    m_ui->progress->show();
    setRunning(true);
    if (m_kind == Kind::QualityReport) {
        generateReport(runner);
        return;
    }
    const docgen::Options chosen = options();
    m_ui->status->setText(tr("Reading the catalog…"));
    docgen::loadModel(
        runner, chosen.schemas, this,
        [this, chosen](const docgen::Model &model, const QString &error) {
            if (!error.isEmpty()) {
                fail(error.trimmed());
                return;
            }
            m_ui->status->setText(tr("Writing the pages…"));
            m_done = std::make_shared<std::atomic<int>>(0);
            m_total = std::make_shared<std::atomic<int>>(0);
            docgen::Options run = chosen;
            run.output.progress = [done = m_done, total = m_total](int d, int t) {
                *done = d;
                *total = t;
            };
            // Graphviz alone can take a while on a big schema.
            auto *watcher = new QFutureWatcher<docgen::WriteResult>(this);
            connect(watcher, &QFutureWatcher<docgen::WriteResult>::finished, this, [this, watcher] {
                watcher->deleteLater();
                finish(watcher->result());
            });
            watcher->setFuture(
                QtConcurrent::run([model, run] { return docgen::generate(model, run); }));
        });
}

void DocGenWizard::generateReport(pg::QueryRunner *runner)
{
    const quality::Options chosen = reportOptions();
    m_ui->status->setText(chosen.health ? tr("Reading the catalog and the statistics…")
                                        : tr("Reading the catalog…"));
    quality::loadReport(
        runner, chosen.schemas, chosen.health, this,
        [this, chosen](const quality::Loaded &loaded, const QString &error) {
            if (!error.isEmpty()) {
                fail(error.trimmed());
                return;
            }
            m_ui->status->setText(tr("Writing the report…"));
            m_done = std::make_shared<std::atomic<int>>(0);
            m_total = std::make_shared<std::atomic<int>>(0);
            quality::Options run = chosen;
            run.output.progress = [done = m_done, total = m_total](int d, int t) {
                *done = d;
                *total = t;
            };
            auto *watcher = new QFutureWatcher<quality::Generated>(this);
            connect(watcher, &QFutureWatcher<quality::Generated>::finished, this, [this, watcher] {
                watcher->deleteLater();
                const quality::Generated generated = watcher->result();
                const auto n
                    = [&](quality::Severity s) { return quality::count(generated.topics, s); };
                finish(generated.written,
                       tr("%1 critical, %2 warnings, %3 notices.")
                           .arg(n(quality::Severity::Critical))
                           .arg(n(quality::Severity::Warning))
                           .arg(n(quality::Severity::Notice)));
            });
            watcher->setFuture(
                QtConcurrent::run([loaded, run] { return quality::generate(loaded, run); }));
        });
}

void DocGenWizard::finish(const docgen::WriteResult &result, const QString &summary)
{
    m_result = result;
    if (!result.ok()) {
        fail(result.error);
        return;
    }
    m_succeeded = true;
    m_ui->progress->setRange(0, 1);
    m_ui->progress->setValue(1);
    setRunning(false);
    const QString folder = QDir::toNativeSeparators(m_ui->output->text().trimmed());
    QString text = result.pages == 1 ? tr("Wrote 1 page into %1.").arg(folder)
                                     : tr("Wrote %1 pages into %2.").arg(result.pages).arg(folder);
    if (!summary.isEmpty())
        text += QLatin1Char(' ') + summary;
    for (const QString &warning : result.warnings)
        text += QLatin1Char('\n') + warning;
    m_ui->status->setText(text);
    if (m_ui->openWhenDone->isChecked()) {
        // A website opens at its home page; anything else is for another
        // tool, so its folder is what to show.
        const bool html = options().output.format == docgen::Format::Html;
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            html ? result.entryPoint : QFileInfo(result.entryPoint).absolutePath()));
    }
    Q_EMIT generated(true);
}

void DocGenWizard::fail(const QString &message)
{
    m_ui->progress->hide();
    setRunning(false);
    m_ui->status->setText(
        tr("Failed: %1\n\nGo back to change what to write, or where.").arg(message));
    Q_EMIT generated(false);
}

void DocGenWizard::reject()
{
    // The pages are being written on another thread; let it finish.
    if (!m_running)
        QWizard::reject();
}

void DocGenWizard::saveSettings() const
{
    QSettings settings;
    const QString group = settingsGroup();
    settings.setValue(group + QStringLiteral("format"), m_ui->format->currentData());
    settings.setValue(group + QStringLiteral("directory"), m_ui->output->text().trimmed());
    settings.setValue(group + QStringLiteral("antoraName"), m_ui->antoraName->text().trimmed());
    for (const QCheckBox *box : remembered())
        settings.setValue(group + box->objectName(), box->isChecked());
}

} // namespace slonisko
