// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditorTab.h"

#include "ConnectionBrowser.h"
#include "PlanView.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"
#include "catalog/Plan.h"

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QToolBar>
#include <QVBoxLayout>

namespace slonisko {

namespace {

// Converts a 1-based character position in text, as PostgreSQL reports
// error positions, to a byte offset.
qsizetype bytesBefore(const QByteArray &text, int position)
{
    return QString::fromUtf8(text).left(std::max(position - 1, 0)).toUtf8().size();
}

} // namespace

EditorTab::EditorTab(ConnectionBrowser *browser, const QString &name, QWidget *parent)
    : QWidget(parent), m_browser(browser), m_name(name), m_editor(new SqlEditor(this)),
      m_panel(new ResultPanel), m_target(new QComboBox(this)), m_status(new QLabel(this)),
      m_connection(new pg::Connection(this))
{
    auto action
        = [this](const QString &icon, const QString &text, const QKeySequence &key, auto slot) {
              auto *a = new QAction(QIcon::fromTheme(icon), text, this);
              a->setShortcut(key);
              a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
              a->setToolTip(QStringLiteral("%1 (%2)").arg(QString(text).remove(QLatin1Char('&')),
                                                          key.toString(QKeySequence::NativeText)));
              connect(a, &QAction::triggered, this, slot);
              addAction(a);
              return a;
          };
    m_run = action(QStringLiteral("media-playback-start"), tr("&Run Statement"),
                   QKeySequence(Qt::CTRL | Qt::Key_Return), &EditorTab::run);
    m_explain = action(QStringLiteral("view-list-tree"), tr("&Explain"),
                       QKeySequence(Qt::CTRL | Qt::Key_E), [this] { explain(false); });
    m_explainAnalyze
        = action(QStringLiteral("chronometer"), tr("Explain &Analyze"),
                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { explain(true); });
    m_cancel = action(QStringLiteral("process-stop"), tr("&Cancel"),
                      QKeySequence(Qt::CTRL | Qt::Key_Period), &EditorTab::cancel);
    // Ctrl+Enter on the keypad too.
    auto *enter = new QAction(this);
    enter->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter));
    enter->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(enter, &QAction::triggered, this, &EditorTab::run);
    addAction(enter);

    m_target->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_target->setToolTip(tr("Connection this editor runs statements on"));
    connect(m_target, &QComboBox::activated, this, [this](int index) {
        const auto session = m_target->itemData(index, Qt::UserRole).value<QPointer<Session>>();
        setSession(session, m_target->itemData(index, Qt::UserRole + 1).toString());
    });

    auto *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->addWidget(m_target);
    toolbar->addSeparator();
    toolbar->addAction(m_run);
    toolbar->addAction(m_explain);
    toolbar->addAction(m_explainAnalyze);
    toolbar->addAction(m_cancel);
    toolbar->addSeparator();
    toolbar->addWidget(m_status);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(m_editor);

    connect(m_connection, &pg::Connection::resultReady, this, &EditorTab::onResult);
    connect(m_connection, &pg::Connection::queryFinished, this, &EditorTab::onFinished);
    connect(m_connection, &pg::Connection::stateChanged, this, &EditorTab::onConnectionState);
    connect(m_connection, &pg::Connection::notice, this,
            [this](const QString &message) { m_panel->log(message); });
    connect(m_browser, &ConnectionBrowser::sessionsChanged, this, &EditorTab::updateSessions);
    m_editor->setSnapshotProvider(
        [this] { return m_session ? m_session->snapshot(m_database) : catalog::SnapshotPtr(); });

    updateSessions();
    updateActions();
}

EditorTab::~EditorTab()
{
    m_connection->disconnect(this);
    delete m_panel; // Lives in the main window's result area, not in this widget.
}

QString EditorTab::title() const
{
    if (!m_session)
        return m_name;
    const QString db = m_database.isEmpty() || m_database == m_session->profile().database
        ? QString()
        : QLatin1Char('/') + m_database;
    return m_name + QStringLiteral(" · ") + m_session->profile().displayName() + db;
}

void EditorTab::setSession(Session *session, const QString &database)
{
    if (isRunning())
        cancel();
    m_jobs.clear();
    m_current.reset();
    m_connection->close();

    m_session = session;
    m_database = database;
    if (m_session) {
        m_panel->log(tr("Connecting to %1…").arg(title()));
        m_connection->open(m_session->conninfo(m_database));
    }
    updateSessions();
    updateActions();
    updateStatus();
    Q_EMIT titleChanged();
}

void EditorTab::updateSessions()
{
    // A session that went away takes the editor's connection with it: its
    // SSH tunnel is gone.
    if (m_session && m_session->state() != Session::State::Connected) {
        setSession(nullptr);
        return;
    }
    const bool gone = !m_session && m_connection->state() != pg::Connection::State::Disconnected;
    if (gone)
        m_connection->close();

    QSignalBlocker block(m_target);
    m_target->clear();
    m_target->addItem(tr("(not connected)"));
    for (Session *s : m_browser->connectedSessions()) {
        const QString db = m_session == s ? m_database : QString();
        const QString label = db.isEmpty() || db == s->profile().database
            ? s->profile().displayName()
            : s->profile().displayName() + QLatin1Char('/') + db;
        m_target->addItem(label);
        const int i = m_target->count() - 1;
        m_target->setItemData(i, QVariant::fromValue(QPointer<Session>(s)), Qt::UserRole);
        m_target->setItemData(i, db, Qt::UserRole + 1);
        if (m_session == s)
            m_target->setCurrentIndex(i);
    }
}

void EditorTab::updateActions()
{
    const bool ready = m_connection->state() == pg::Connection::State::Ready && !isRunning();
    m_run->setEnabled(ready);
    m_explain->setEnabled(ready);
    m_explainAnalyze->setEnabled(ready);
    m_cancel->setEnabled(isRunning());
}

void EditorTab::updateStatus()
{
    QString text;
    switch (m_connection->state()) {
    case pg::Connection::State::Disconnected:
        text = tr("Not connected");
        break;
    case pg::Connection::State::Connecting:
        text = tr("Connecting…");
        break;
    case pg::Connection::State::Failed:
        text = tr("Connection failed");
        break;
    case pg::Connection::State::Busy:
        text = tr("Running…");
        break;
    case pg::Connection::State::Ready:
        switch (m_connection->transactionStatus()) {
        case PQTRANS_INTRANS:
            text = tr("In transaction");
            break;
        case PQTRANS_INERROR:
            text = tr("Transaction failed: roll back");
            break;
        default:
            text = tr("Idle");
            break;
        }
        break;
    }
    m_status->setText(text);
    m_status->setStyleSheet(m_connection->transactionStatus() == PQTRANS_INERROR
                                    || m_connection->state() == pg::Connection::State::Failed
                                ? QStringLiteral("color: #c62828;")
                                : m_connection->transactionStatus() == PQTRANS_INTRANS
                                ? QStringLiteral("color: #b26a00;")
                                : QString());
}

void EditorTab::onConnectionState(pg::Connection::State state)
{
    if (state == pg::Connection::State::Ready && !isRunning()) {
        m_panel->log(tr("Connected to %1.").arg(title()));
        if (m_session)
            m_session->snapshot(m_database); // Loads the completion data.
    }
    if (state == pg::Connection::State::Failed) {
        m_panel->log(m_connection->errorMessage(), true);
        if (isRunning()) {
            m_panel->results()->showError(m_connection->errorMessage());
            finishAll();
        }
    }
    updateActions();
    updateStatus();
}

// Running.

void EditorTab::run()
{
    if (m_connection->state() != pg::Connection::State::Ready || isRunning())
        return;
    std::deque<Job> jobs;
    for (const ScriptPiece &piece : m_editor->piecesToRun()) {
        if (piece.psqlCommand) {
            m_panel->log(tr("Skipped psql command: %1").arg(preview(piece.sql)));
            continue;
        }
        Job job;
        job.sql = piece.sql;
        job.copyData = piece.copyData;
        job.offset = piece.offset;
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        m_panel->log(tr("Nothing to run here: put the cursor in a statement or select some."));
        return;
    }
    m_panel->showResults();
    start(std::move(jobs));
}

void EditorTab::explain(bool analyze)
{
    if (m_connection->state() != pg::Connection::State::Ready || isRunning())
        return;
    const auto pieces = m_editor->piecesToRun();
    if (pieces.size() != 1 || pieces.front().psqlCommand || pieces.front().copyData) {
        m_panel->plan()->showMessage(
            tr("Put the cursor in one SQL statement, or select one, to explain it."), true);
        m_panel->showPlan();
        return;
    }

    const QByteArray prefix
        = analyze ? "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) " : "EXPLAIN (FORMAT JSON) ";
    Job job;
    job.kind = Job::Kind::Explain;
    job.sql = prefix + pieces.front().sql;
    job.offset = pieces.front().offset;
    job.prefixChars = int(prefix.size());
    job.analyze = analyze;

    // ANALYZE really runs the statement: roll back whatever it changes.
    std::deque<Job> jobs;
    auto silent = [](const QByteArray &sql, bool always) {
        Job j;
        j.kind = Job::Kind::Silent;
        j.sql = sql;
        j.always = always;
        return j;
    };
    if (!analyze) {
        jobs.push_back(job);
    } else if (m_connection->transactionStatus() == PQTRANS_IDLE) {
        jobs = {silent("BEGIN", false), job, silent("ROLLBACK", true)};
    } else if (m_connection->transactionStatus() == PQTRANS_INTRANS) {
        jobs = {silent("SAVEPOINT slonisko_explain", false), job,
                silent("ROLLBACK TO SAVEPOINT slonisko_explain", true),
                silent("RELEASE SAVEPOINT slonisko_explain", true)};
    } else {
        m_panel->plan()->showMessage(
            tr("The current transaction has failed; roll it back before running EXPLAIN ANALYZE."),
            true);
        m_panel->showPlan();
        return;
    }
    m_panel->plan()->showMessage(tr("Explaining…"));
    m_panel->showPlan();
    start(std::move(jobs));
}

void EditorTab::cancel()
{
    if (isRunning() && m_connection->cancel())
        m_panel->log(tr("Cancelling…"));
}

void EditorTab::start(std::deque<Job> jobs)
{
    m_editor->clearErrors();
    m_jobs = std::move(jobs);
    m_failed = false;
    startNext();
    Q_EMIT runningChanged(true);
}

void EditorTab::startNext()
{
    while (!m_jobs.empty() && m_failed && !m_jobs.front().always)
        m_jobs.pop_front();
    if (m_jobs.empty()) {
        finishAll();
        return;
    }

    m_current = std::move(m_jobs.front());
    m_jobs.pop_front();
    m_rowsShown = false;
    m_rows = 0;
    m_commandTag.clear();
    m_limited = false;
    m_timer.start();
    const int chunk = m_current->kind == Job::Kind::Statement ? 1000 : 0;
    if (!m_connection->execute(m_current->sql, chunk, m_current->copyData)) {
        m_panel->log(tr("Could not run the statement: %1").arg(m_connection->errorMessage()), true);
        finishAll();
        return;
    }
    updateActions();
    updateStatus();
}

void EditorTab::onResult(const pg::Result &result)
{
    if (!m_current)
        return;
    const Job &job = *m_current;

    if (result.isError()) {
        if (m_limited && result.sqlState() == "57014")
            return; // Our own cancel at the row limit.
        m_failed = true;
        QString message = result.errorMessage();
        m_panel->log(message, true);
        const int position = result.errorPosition();
        if (position > job.prefixChars)
            markError(job, position - job.prefixChars);
        if (job.kind == Job::Kind::Explain) {
            m_panel->plan()->showMessage(message, true);
        } else if (job.kind == Job::Kind::Statement && !m_rowsShown) {
            m_panel->results()->showError(message);
            m_panel->showResults();
        }
        return;
    }

    switch (job.kind) {
    case Job::Kind::Explain:
        if (result.status() == PGRES_TUPLES_OK && result.rowCount() > 0) {
            QString error;
            if (const auto plan = catalog::parsePlan(result.value(0, 0), &error))
                m_panel->plan()->setPlan(*plan);
            else
                m_panel->plan()->showMessage(error, true);
        }
        break;
    case Job::Kind::Statement:
        // COPY results report columns too, but carry no rows.
        if ((result.status() == PGRES_TUPLES_OK || result.status() == PGRES_TUPLES_CHUNK)
            && result.columnCount() > 0) {
            if (!m_rowsShown) {
                m_rowsShown = true;
                m_panel->results()->setRerun({});
                m_panel->results()->begin(preview(job.sql));
            }
            m_panel->results()->append(result);
            m_rows += result.rowCount();
            if (m_rows >= m_rowLimit && !m_limited && m_connection->cancel())
                m_limited = true;
        }
        if (result.status() != PGRES_TUPLES_CHUNK)
            m_commandTag = result.commandTag();
        break;
    case Job::Kind::Silent:
        break;
    }
}

void EditorTab::onFinished()
{
    if (!m_current)
        return;
    const Job job = std::move(*m_current);
    m_current.reset();
    const qint64 ms = m_timer.elapsed();

    if (job.kind == Job::Kind::Statement) {
        // DDL changes what completion knows about, once others can see it.
        const bool ddl = m_commandTag.startsWith("CREATE") || m_commandTag.startsWith("ALTER")
            || m_commandTag.startsWith("DROP");
        if (m_session && !m_failed
            && ((ddl && m_connection->transactionStatus() == PQTRANS_IDLE)
                || m_commandTag == "COMMIT"))
            m_session->reloadSnapshot(m_database);
        QString line = preview(job.sql);
        if (m_limited)
            line += tr(": stopped after %n row(s)", nullptr, int(m_rows));
        else if (m_rowsShown) // In chunked mode libpq leaves the command tag empty.
            line += tr(": %n row(s)", nullptr, int(m_rows));
        else if (!m_commandTag.isEmpty())
            line += QStringLiteral(": ") + QString::fromUtf8(m_commandTag);
        line += tr(" (%1 ms)").arg(ms);
        if (!m_failed || m_limited)
            m_panel->log(line);
        if (m_rowsShown)
            m_panel->results()->finish(
                m_limited ? tr("First %n row(s), %1 ms", nullptr, int(m_rows)).arg(ms)
                          : tr("%n row(s), %1 ms", nullptr, int(m_rows)).arg(ms));
        else if (!m_failed)
            m_panel->results()->showMessage(
                m_commandTag.isEmpty()
                    ? tr("Done in %1 ms.").arg(ms)
                    : tr("%1 in %2 ms.").arg(QString::fromUtf8(m_commandTag)).arg(ms));
    } else if (job.kind == Job::Kind::Explain && !m_failed) {
        m_panel->log(
            tr("Explained: %1 (%2 ms)").arg(preview(job.sql.mid(job.prefixChars))).arg(ms));
    }
    startNext();
}

void EditorTab::finishAll()
{
    m_jobs.clear();
    m_current.reset();
    updateActions();
    updateStatus();
    Q_EMIT runningChanged(false);
}

void EditorTab::markError(const Job &job, int position)
{
    if (job.offset < 0)
        return;
    const QByteArray userText = job.sql.mid(job.prefixChars);
    m_editor->markError(job.offset + bytesBefore(userText, position));
}

QString EditorTab::preview(const QByteArray &sql)
{
    QString text = QString::fromUtf8(sql).simplified();
    if (text.size() > 60)
        text = text.left(60) + QChar(0x2026);
    return text;
}

} // namespace slonisko
