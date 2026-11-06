// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SchemaDiffWizard.h"

#include "BrowserModel.h"
#include "CalloutLabel.h"
#include "ConnectionBrowser.h"
#include "Session.h"
#include "SqlEditor.h"

#include "ui_SchemaDiffWizard.h"

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>

#include <set>

namespace slonisko {

namespace {

const QString SettingsGroup = QStringLiteral("schemadiff/");

QString kindText(schemadiff::ChangeKind kind)
{
    switch (kind) {
    case schemadiff::ChangeKind::Added:
        return SchemaDiffWizard::tr("Added");
    case schemadiff::ChangeKind::Removed:
        return SchemaDiffWizard::tr("Removed");
    case schemadiff::ChangeKind::Changed:
        break;
    }
    return SchemaDiffWizard::tr("Changed");
}

} // namespace

SchemaDiffWizard::SchemaDiffWizard(ConnectionBrowser *browser, QWidget *parent)
    : QWizard(parent), m_ui(std::make_unique<Ui::SchemaDiffWizard>()), m_browser(browser)
{
    m_ui->setupUi(this);
    m_target = {m_ui->targetProfile, m_ui->targetDatabase, m_ui->targetState};
    m_reference = {m_ui->referenceProfile, m_ui->referenceDatabase, m_ui->referenceState};
    m_ui->script->setReadOnly(true);
    m_ui->resultSplitter->setStretchFactor(0, 1);
    m_ui->resultSplitter->setStretchFactor(1, 2);

    m_ui->connectionsPage->setComplete([this] {
        if (targetProfile().isNull() || referenceProfile().isNull())
            return false;
        return targetProfile() != referenceProfile() || targetDatabase() != referenceDatabase();
    });
    m_ui->schemasPage->setComplete([this] {
        for (int i = 0; i < m_ui->schemas->count(); ++i) {
            if (m_ui->schemas->item(i)->checkState() == Qt::Checked)
                return true;
        }
        return false;
    });
    m_ui->schemasPage->setButtonText(QWizard::NextButton, tr("&Compare"));
    m_ui->resultPage->setComplete([this] { return !m_comparing; });

    const QSettings settings;
    m_ui->concurrentIndexes->setChecked(
        settings.value(SettingsGroup + QStringLiteral("concurrentIndexes"), false).toBool());
    m_ui->transaction->setChecked(
        settings.value(SettingsGroup + QStringLiteral("transaction"), true).toBool());

    fillProfiles();

    for (const Side *side : {&m_target, &m_reference}) {
        const Side s = *side;
        connect(s.profile, &QComboBox::currentIndexChanged, this, [this, s] { profileChanged(s); });
        connect(s.database, &QComboBox::currentTextChanged, m_ui->connectionsPage,
                &DocGenPage::updateComplete);
    }
    if (m_browser)
        connect(m_browser, &ConnectionBrowser::sessionsChanged, this,
                &SchemaDiffWizard::updateConnections);
    connect(m_ui->schemas, &QListWidget::itemChanged, m_ui->schemasPage,
            &DocGenPage::updateComplete);
    const auto checkAll = [this](Qt::CheckState state) {
        for (int i = 0; i < m_ui->schemas->count(); ++i)
            m_ui->schemas->item(i)->setCheckState(state);
    };
    connect(m_ui->allSchemas, &QPushButton::clicked, this, [checkAll] { checkAll(Qt::Checked); });
    connect(m_ui->noSchemas, &QPushButton::clicked, this, [checkAll] { checkAll(Qt::Unchecked); });
    connect(m_ui->changes, &QTreeWidget::currentItemChanged, this, &SchemaDiffWizard::showChange);
    connect(m_ui->transaction, &QCheckBox::toggled, this, &SchemaDiffWizard::updateScript);
    // Changes the statements, not only how they are wrapped.
    connect(m_ui->concurrentIndexes, &QCheckBox::toggled, this, &SchemaDiffWizard::showDiff);
    connect(m_ui->openInEditor, &QPushButton::clicked, this, [this] {
        if (Session *session = targetSession())
            Q_EMIT scriptRequested(session, targetDatabase(), m_script);
    });
    connect(m_ui->saveScript, &QPushButton::clicked, this, &SchemaDiffWizard::saveScript);
    connect(m_ui->copyScript, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(m_script); });

    updateConnections();
}

SchemaDiffWizard::~SchemaDiffWizard() = default;

void SchemaDiffWizard::fillProfiles()
{
    if (!m_browser)
        return;
    std::vector<config::ConnectionProfile> profiles = m_browser->model()->profiles();
    std::ranges::sort(profiles, [](const auto &a, const auto &b) {
        return a.displayName().compare(b.displayName(), Qt::CaseInsensitive) < 0;
    });
    for (const Side *side : {&m_target, &m_reference}) {
        const QSignalBlocker blocker(side->profile);
        for (const config::ConnectionProfile &profile : profiles)
            side->profile->addItem(profile.displayName(), profile.id);
        side->profile->setCurrentIndex(-1);
    }

    // What was compared last time, if its connections are still there;
    // nothing is guessed, so neither side is ever a database not chosen.
    const QSettings settings;
    for (const auto &[side, key] : {std::pair(&m_target, QStringLiteral("target")),
                                    std::pair(&m_reference, QStringLiteral("reference"))}) {
        const QUuid id = settings.value(SettingsGroup + key).toUuid();
        if (!id.isNull() && side->profile->findData(id) >= 0)
            choose(*side, id,
                   settings.value(SettingsGroup + key + QStringLiteral("Database")).toString());
    }
}

void SchemaDiffWizard::setTarget(const QUuid &profile, const QString &database)
{
    choose(m_target, profile, database);
}

void SchemaDiffWizard::setReference(const QUuid &profile, const QString &database)
{
    choose(m_reference, profile, database);
}

void SchemaDiffWizard::choose(const Side &side, const QUuid &profile, const QString &database)
{
    {
        const QSignalBlocker blocker(side.profile);
        side.profile->setCurrentIndex(side.profile->findData(profile));
    }
    profileChanged(side);
    if (!database.isEmpty())
        side.database->setEditText(database);
    updateConnections();
}

QUuid SchemaDiffWizard::targetProfile() const
{
    return m_target.profile->currentData().toUuid();
}

QUuid SchemaDiffWizard::referenceProfile() const
{
    return m_reference.profile->currentData().toUuid();
}

QString SchemaDiffWizard::targetDatabase() const
{
    return databaseOf(m_target);
}

QString SchemaDiffWizard::referenceDatabase() const
{
    return databaseOf(m_reference);
}

QString SchemaDiffWizard::databaseOf(const Side &side) const
{
    const QString database = side.database->currentText().trimmed();
    if (!database.isEmpty() || !m_browser)
        return database;
    const config::ConnectionProfile *profile
        = m_browser->model()->profile(side.profile->currentData().toUuid());
    return profile ? profile->database : QString();
}

Session *SchemaDiffWizard::session(const Side &side) const
{
    return m_browser ? m_browser->model()->session(side.profile->currentData().toUuid()) : nullptr;
}

Session *SchemaDiffWizard::targetSession() const
{
    return session(m_target);
}

Session *SchemaDiffWizard::referenceSession() const
{
    return session(m_reference);
}

QString SchemaDiffWizard::sideName(const Side &side) const
{
    return QStringLiteral("%1 (%2)").arg(databaseOf(side), side.profile->currentText());
}

void SchemaDiffWizard::profileChanged(const Side &side)
{
    const QSignalBlocker blocker(side.database);
    side.database->clear();
    const config::ConnectionProfile *profile
        = m_browser ? m_browser->model()->profile(side.profile->currentData().toUuid()) : nullptr;
    side.database->setEditText(profile ? profile->database : QString());
    updateConnections();
}

void SchemaDiffWizard::updateConnections()
{
    bool failed = false;
    bool connected = true;
    for (const Side *side : {&m_target, &m_reference}) {
        const Session *s = session(*side);
        const Session::State state = s ? s->state() : Session::State::Disconnected;
        switch (state) {
        case Session::State::Connected:
            side->state->setText(tr("connected"));
            // The databases of the server, to choose from, once.
            if (side->database->count() == 0)
                loadDatabases(*side);
            break;
        case Session::State::Connecting:
            side->state->setText(tr("connecting…"));
            break;
        case Session::State::Failed:
            side->state->setText(tr("failed"));
            failed = true;
            break;
        case Session::State::Disconnected:
            side->state->setText(tr("not connected"));
            break;
        }
        connected = connected && state == Session::State::Connected;
    }

    m_ui->connectionsStatus->setError(false);
    if (targetProfile() == referenceProfile() && !targetProfile().isNull()
        && targetDatabase() == referenceDatabase()) {
        m_ui->connectionsStatus->setError(true);
        m_ui->connectionsStatus->setText(tr("Target and Reference are the same database."));
    } else if (m_advanceWhenConnected && failed) {
        m_advanceWhenConnected = false;
        m_ui->connectionsStatus->setError(true);
        m_ui->connectionsStatus->setText(tr("Could not connect."));
    } else if (m_advanceWhenConnected && connected) {
        m_advanceWhenConnected = false;
        m_ui->connectionsStatus->clear();
        if (currentPage() == m_ui->connectionsPage)
            next();
    } else if (!m_advanceWhenConnected) {
        m_ui->connectionsStatus->setText(
            connected ? QString() : tr("Connections not open yet are opened with Next."));
    }
    m_ui->connectionsPage->updateComplete();
}

void SchemaDiffWizard::loadDatabases(const Side &side)
{
    Session *s = session(side);
    pg::QueryRunner *runner = s ? s->runner() : nullptr;
    if (!runner)
        return;
    const QPointer<QComboBox> combo = side.database;
    runner->run(schemadiff::databaseListQuery(), this, [combo](const pg::QueryOutcome &outcome) {
        if (!combo || !outcome.ok() || outcome.results.empty())
            return;
        const QString chosen = combo->currentText();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(docgen::parseSchemaList(outcome.results.back()));
        combo->setEditText(chosen);
    });
}

bool SchemaDiffWizard::validateCurrentPage()
{
    if (currentPage() != m_ui->connectionsPage)
        return QWizard::validateCurrentPage();
    const Session *target = targetSession();
    const Session *reference = referenceSession();
    const auto connected
        = [](const Session *s) { return s && s->state() == Session::State::Connected; };
    if (connected(target) && connected(reference)) {
        saveSettings();
        return QWizard::validateCurrentPage();
    }
    if (!m_browser)
        return false;
    // Opened as the browser opens them, password prompts and all; the page
    // moves on by itself once both are.
    m_advanceWhenConnected = true;
    m_ui->connectionsStatus->setError(false);
    m_ui->connectionsStatus->setText(tr("Connecting…"));
    for (const QUuid &id : {targetProfile(), referenceProfile()}) {
        const Session *s = m_browser->model()->session(id);
        if (!s || s->state() != Session::State::Connecting)
            m_browser->connectProfile(id);
    }
    updateConnections();
    return false;
}

void SchemaDiffWizard::initializePage(int id)
{
    QWizard::initializePage(id);
    if (page(id) == m_ui->schemasPage)
        loadSchemas();
    else if (page(id) == m_ui->resultPage)
        compare();
}

void SchemaDiffWizard::loadSchemas()
{
    // What was unchecked stays so when coming back.
    QSet<QString> unchecked;
    for (int i = 0; i < m_ui->schemas->count(); ++i) {
        if (m_ui->schemas->item(i)->checkState() != Qt::Checked)
            unchecked.insert(m_ui->schemas->item(i)->text());
    }
    m_ui->schemas->clear();
    m_ui->schemasPage->updateComplete();

    Session *target = targetSession();
    Session *reference = referenceSession();
    pg::QueryRunner *runners[] = {target ? target->runner(targetDatabase()) : nullptr,
                                  reference ? reference->runner(referenceDatabase()) : nullptr};
    if (!runners[0] || !runners[1]) {
        m_ui->schemasStatus->setError(true);
        m_ui->schemasStatus->setText(tr("Not connected."));
        return;
    }
    m_ui->schemasStatus->setError(false);
    m_ui->schemasStatus->setText(tr("Reading…"));
    auto names = std::make_shared<std::set<QString>>();
    auto pending = std::make_shared<int>(2);
    auto error = std::make_shared<QString>();
    const int generation = ++m_generation;
    for (pg::QueryRunner *runner : runners) {
        runner->run(
            docgen::schemaListQuery(), this,
            [this, names, pending, error, unchecked, generation](const pg::QueryOutcome &o) {
                if (generation != m_generation)
                    return;
                if (!o.ok() || o.results.empty())
                    *error = o.error.trimmed();
                else
                    for (const QString &name : docgen::parseSchemaList(o.results.back()))
                        names->insert(name);
                if (--*pending > 0)
                    return;
                if (!error->isEmpty()) {
                    m_ui->schemasStatus->setError(true);
                    m_ui->schemasStatus->setText(*error);
                    return;
                }
                const QSignalBlocker blocker(m_ui->schemas);
                for (const QString &name : *names) {
                    auto *item = new QListWidgetItem(name, m_ui->schemas);
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(unchecked.contains(name) ? Qt::Unchecked : Qt::Checked);
                }
                const int count = m_ui->schemas->count();
                m_ui->schemasStatus->setText(count == 1 ? tr("1 schema")
                                                        : tr("%1 schemas").arg(count));
                m_ui->schemasPage->updateComplete();
            });
    }
}

void SchemaDiffWizard::compare()
{
    m_targetModel.reset();
    m_referenceModel.reset();
    m_diff = {};
    m_script.clear();
    m_ui->changes->clear();
    m_ui->script->setText(QString());
    for (QWidget *w :
         {static_cast<QWidget *>(m_ui->openInEditor), static_cast<QWidget *>(m_ui->saveScript),
          static_cast<QWidget *>(m_ui->copyScript)})
        w->setEnabled(false);
    m_ui->status->setError(false);

    Session *target = targetSession();
    Session *reference = referenceSession();
    pg::QueryRunner *targetRunner = target ? target->runner(targetDatabase()) : nullptr;
    pg::QueryRunner *referenceRunner = reference ? reference->runner(referenceDatabase()) : nullptr;
    if (!targetRunner || !referenceRunner) {
        fail(tr("Not connected."));
        return;
    }

    docgen::SchemaFilter filter;
    QStringList checked;
    for (int i = 0; i < m_ui->schemas->count(); ++i) {
        if (m_ui->schemas->item(i)->checkState() == Qt::Checked)
            checked << m_ui->schemas->item(i)->text();
    }
    // All of them, schemas made after the list was read included.
    if (checked.size() < m_ui->schemas->count())
        filter.include = checked;

    m_comparing = true;
    m_succeeded = false;
    m_ui->resultPage->updateComplete();
    m_ui->status->setText(tr("Reading both catalogs…"));
    const int generation = ++m_generation;
    const auto loaded = [this, generation](std::optional<docgen::Model> &slot) {
        return [this, generation, &slot](const docgen::Model &model, const QString &error) {
            if (generation != m_generation || !m_comparing)
                return;
            if (!error.isEmpty()) {
                fail(error.trimmed());
                return;
            }
            slot = model;
            if (m_targetModel && m_referenceModel) {
                m_comparing = false;
                m_succeeded = true;
                showDiff();
                m_ui->resultPage->updateComplete();
                Q_EMIT compared(true);
            }
        };
    };
    schemadiff::loadModel(targetRunner, filter, this, loaded(m_targetModel));
    schemadiff::loadModel(referenceRunner, filter, this, loaded(m_referenceModel));
}

void SchemaDiffWizard::showDiff()
{
    if (!m_targetModel || !m_referenceModel)
        return;
    schemadiff::Options options;
    options.concurrentIndexes = m_ui->concurrentIndexes->isChecked();
    m_diff = schemadiff::compare(*m_targetModel, *m_referenceModel, options);

    m_ui->changes->clear();
    int added = 0;
    int removed = 0;
    int hazardous = 0;
    for (std::size_t i = 0; i < m_diff.changes.size(); ++i) {
        const schemadiff::Change &change = m_diff.changes[i];
        QStringList hazards;
        QStringList messages;
        for (const schemadiff::Hazard &hazard : change.hazards) {
            if (!hazards.contains(schemadiff::hazardName(hazard.type)))
                hazards << schemadiff::hazardName(hazard.type);
            messages << hazard.message;
        }
        auto *item = new QTreeWidgetItem(
            m_ui->changes,
            {kindText(change.kind),
             schemadiff::typeName(change.type) + QLatin1Char(' ') + change.name, change.details,
             hazards.join(QStringLiteral(", "))});
        item->setData(0, Qt::UserRole, int(i));
        if (!messages.isEmpty())
            item->setToolTip(3, messages.join(QLatin1Char('\n')));
        added += change.kind == schemadiff::ChangeKind::Added;
        removed += change.kind == schemadiff::ChangeKind::Removed;
        hazardous += !change.hazards.empty();
    }
    for (int column = 0; column < m_ui->changes->columnCount() - 1; ++column)
        m_ui->changes->resizeColumnToContents(column);

    const int count = int(m_diff.changes.size());
    if (count == 0) {
        m_ui->status->setTone(CalloutLabel::Tone::Success);
        m_ui->status->setText(tr("The schemas are the same."));
    } else {
        QString text = tr("%1 differences: %2 added, %3 removed, %4 changed.")
                           .arg(count)
                           .arg(added)
                           .arg(removed)
                           .arg(count - added - removed);
        if (hazardous > 0)
            text += QLatin1Char(' ')
                + (hazardous == 1
                       ? tr("1 of them has hazards, marked in the script.")
                       : tr("%1 of them have hazards, marked in the script.").arg(hazardous));
        m_ui->status->setTone(CalloutLabel::Tone::Plain);
        m_ui->status->setText(text);
    }
    updateScript();
}

void SchemaDiffWizard::updateScript()
{
    if (!m_succeeded)
        return;
    // Statements that cannot run in a transaction make the choice moot.
    m_ui->transaction->setEnabled(m_diff.transactional());
    schemadiff::ScriptOptions options;
    options.targetName = sideName(m_target);
    options.referenceName = sideName(m_reference);
    options.transaction = m_ui->transaction->isChecked();
    m_script = schemadiff::script(m_diff, options, &m_lines);
    m_ui->script->setText(m_script);
    const bool any = !m_diff.isEmpty();
    m_ui->openInEditor->setEnabled(any && targetSession());
    m_ui->saveScript->setEnabled(any);
    m_ui->copyScript->setEnabled(any);
    saveSettings();
}

// Shows where in the script the change is made.
void SchemaDiffWizard::showChange()
{
    const QTreeWidgetItem *item = m_ui->changes->currentItem();
    if (!item)
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || std::size_t(index) >= m_lines.size() || m_lines[index].empty())
        return;
    const auto [first, last] = m_lines[index].front();
    m_ui->script->setSelection(first, 0, last + 1, 0);
    m_ui->script->ensureLineVisible(last);
    m_ui->script->ensureLineVisible(first);
}

void SchemaDiffWizard::fail(const QString &message)
{
    m_comparing = false;
    m_succeeded = false;
    m_ui->status->setError(true);
    m_ui->status->setText(tr("Failed: %1").arg(message));
    m_ui->resultPage->updateComplete();
    Q_EMIT compared(false);
}

void SchemaDiffWizard::saveScript()
{
    QSettings settings;
    const QString last = settings.value(SettingsGroup + QStringLiteral("directory")).toString();
    const QString suggested = QDir(last.isEmpty() ? QDir::homePath() : last)
                                  .filePath(targetDatabase() + QStringLiteral("-align.sql"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Script"), suggested,
                                                      tr("SQL scripts (*.sql);;All files (*)"));
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(m_script.toUtf8()) < 0 || !file.commit()) {
        QMessageBox::warning(this, tr("Save Script"),
                             tr("Could not write %1:\n%2").arg(path, file.errorString()));
        return;
    }
    settings.setValue(SettingsGroup + QStringLiteral("directory"), QFileInfo(path).absolutePath());
}

void SchemaDiffWizard::saveSettings() const
{
    QSettings settings;
    settings.setValue(SettingsGroup + QStringLiteral("target"), targetProfile());
    settings.setValue(SettingsGroup + QStringLiteral("targetDatabase"), targetDatabase());
    settings.setValue(SettingsGroup + QStringLiteral("reference"), referenceProfile());
    settings.setValue(SettingsGroup + QStringLiteral("referenceDatabase"), referenceDatabase());
    settings.setValue(SettingsGroup + QStringLiteral("concurrentIndexes"),
                      m_ui->concurrentIndexes->isChecked());
    settings.setValue(SettingsGroup + QStringLiteral("transaction"),
                      m_ui->transaction->isChecked());
}

} // namespace slonisko
