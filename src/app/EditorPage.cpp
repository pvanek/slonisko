// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditorPage.h"

#include "ConnectionBrowser.h"
#include "Icons.h"
#include "PlanView.h"
#include "ResultPanel.h"
#include "ResultModel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"
#include "SaveChangesDialog.h"
#include "catalog/Editing.h"
#include "catalog/Plan.h"

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
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

EditorPage::EditorPage(ConnectionBrowser *browser, const QString &name, QWidget *parent)
    : WorkspacePage(parent), m_browser(browser), m_name(name), m_editor(new SqlEditor(this)),
      m_panel(new ResultPanel(this)), m_target(new QComboBox(this)), m_status(new QLabel(this)),
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
                   QKeySequence(Qt::CTRL | Qt::Key_Return), &EditorPage::run);
    m_explain = action(QStringLiteral("view-list-tree"), tr("&Explain"),
                       QKeySequence(Qt::CTRL | Qt::Key_E), [this] { explain(false); });
    m_explainAnalyze
        = action(QStringLiteral("chronometer"), tr("Explain &Analyze"),
                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { explain(true); });
    m_cancel = action(QStringLiteral("process-stop"), tr("&Cancel"),
                      QKeySequence(Qt::CTRL | Qt::Key_Period), &EditorPage::cancel);
    // Ctrl+Enter on the keypad too.
    auto *enter = new QAction(this);
    enter->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter));
    enter->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(enter, &QAction::triggered, this, &EditorPage::run);
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
    // The editor above its results, in one page.
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_editor);
    splitter->addWidget(m_panel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter);

    connect(m_connection, &pg::Connection::resultReady, this, &EditorPage::onResult);
    connect(m_connection, &pg::Connection::queryFinished, this, &EditorPage::onFinished);
    connect(m_connection, &pg::Connection::stateChanged, this, &EditorPage::onConnectionState);
    connect(m_connection, &pg::Connection::notice, this,
            [this](const QString &message) { m_panel->log(message); });
    connect(m_browser, &ConnectionBrowser::sessionsChanged, this, &EditorPage::updateSessions);
    connect(m_editor, &QsciScintilla::modificationChanged, this, &EditorPage::titleChanged);
    connect(m_panel->results(), &ResultView::saveRequested, this, [this] { saveChanges(); });
    m_editor->setSnapshotProvider(
        [this] { return m_session ? m_session->snapshot(m_database) : catalog::SnapshotPtr(); });

    updateSessions();
    updateActions();
}

EditorPage::~EditorPage()
{
    m_connection->disconnect(this);
}

QString EditorPage::title() const
{
    QString name = m_filePath.isEmpty() ? m_name : QFileInfo(m_filePath).fileName();
    if (isModified())
        name += QLatin1Char('*');
    if (!m_session)
        return name;
    const QString db = m_database.isEmpty() || m_database == m_session->profile().database
        ? QString()
        : QLatin1Char('/') + m_database;
    return name + QStringLiteral(" · ") + m_session->profile().displayName() + db;
}

QColor EditorPage::color() const
{
    return m_session ? QColor::fromString(m_session->profile().color) : QColor();
}

bool EditorPage::isModified() const
{
    return m_editor->isModified();
}

bool EditorPage::isBlank() const
{
    return m_filePath.isEmpty() && !isModified() && m_editor->length() == 0;
}

bool EditorPage::openFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    // Keep the file's line endings, and type new lines the same way.
    m_editor->setEolMode(bytes.contains("\r\n") ? QsciScintilla::EolWindows
                                                : QsciScintilla::EolUnix);
    m_editor->setText(QString::fromUtf8(bytes));
    m_editor->setModified(false);
    m_editor->setCursorPosition(0);
    m_filePath = QFileInfo(path).absoluteFilePath();
    setToolTip(m_filePath);
    Q_EMIT titleChanged();
    return true;
}

bool EditorPage::saveFile(const QString &path, QString *error)
{
    // Written to a temporary file first, so a failed save leaves the old one.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(m_editor->utf8Text()) < 0
        || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    m_editor->setModified(false);
    m_filePath = QFileInfo(path).absoluteFilePath();
    setToolTip(m_filePath);
    Q_EMIT titleChanged();
    return true;
}

bool EditorPage::save()
{
    if (m_filePath.isEmpty())
        return saveAs();
    QString error;
    if (saveFile(m_filePath, &error))
        return true;
    QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(m_filePath, error));
    return false;
}

bool EditorPage::saveAs()
{
    QSettings settings;
    const QString dir = m_filePath.isEmpty()
        ? settings.value(QStringLiteral("files/lastDirectory")).toString()
        : m_filePath;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save SQL Script"), dir,
                                                      tr("SQL scripts (*.sql);;All files (*)"));
    if (path.isEmpty())
        return false;
    settings.setValue(QStringLiteral("files/lastDirectory"), QFileInfo(path).absolutePath());
    QString error;
    if (saveFile(path, &error))
        return true;
    QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
    return false;
}

bool EditorPage::maybeSave()
{
    if (!isModified())
        return true;
    const QString name = m_filePath.isEmpty() ? m_name : QFileInfo(m_filePath).fileName();
    const auto answer = QMessageBox::question(
        this, tr("Unsaved Changes"), tr("Save the changes to %1?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save();
    return answer == QMessageBox::Discard;
}

void EditorPage::setSession(Session *session, const QString &database)
{
    if (isRunning())
        cancel();
    m_jobs.clear();
    m_current.reset();
    m_connection->close();

    if (m_session)
        m_session->disconnect(this);
    m_session = session;
    m_database = database;
    if (m_session) {
        // New catalog data: completion takes it as it comes; colors need a nudge.
        connect(m_session, &Session::snapshotChanged, this, [this](const QString &db) {
            if (db == (m_database.isEmpty() ? m_session->profile().database : m_database))
                m_editor->refreshSemantics();
        });
        m_panel->log(tr("Connecting to %1…").arg(title()));
        m_connection->open(m_session->conninfo(m_database));
    }
    updateSessions();
    updateActions();
    updateStatus();
    Q_EMIT titleChanged();
}

void EditorPage::updateSessions()
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
        m_target->addItem(Icons::connection(s->profile().color, true), label);
        const int i = m_target->count() - 1;
        m_target->setItemData(i, QVariant::fromValue(QPointer<Session>(s)), Qt::UserRole);
        m_target->setItemData(i, db, Qt::UserRole + 1);
        if (m_session == s)
            m_target->setCurrentIndex(i);
    }
    updateTargetColor();
}

void EditorPage::updateTargetColor()
{
    // Tints the combo box with the connection's color, so it is clear at a
    // glance which server (production!) this editor talks to.
    const QColor c = color();
    QPalette palette; // The application's, untinted.
    if (c.isValid()) {
        for (const QPalette::ColorRole role :
             {QPalette::Button, QPalette::Base, QPalette::Window}) {
            const QColor base = palette.color(role);
            palette.setColor(role,
                             QColor::fromRgbF(base.redF() * 0.75f + c.redF() * 0.25f,
                                              base.greenF() * 0.75f + c.greenF() * 0.25f,
                                              base.blueF() * 0.75f + c.blueF() * 0.25f));
        }
    }
    m_target->setPalette(palette);
    m_target->setProperty("connectionColor", c);
}

void EditorPage::updateActions()
{
    const bool ready = m_connection->state() == pg::Connection::State::Ready && !isRunning();
    m_run->setEnabled(ready);
    m_explain->setEnabled(ready);
    m_explainAnalyze->setEnabled(ready);
    m_cancel->setEnabled(isRunning());
}

void EditorPage::updateStatus()
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

void EditorPage::onConnectionState(pg::Connection::State state)
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

void EditorPage::run()
{
    if (m_connection->state() != pg::Connection::State::Ready || isRunning())
        return;
    std::deque<Job> jobs;
    for (const ScriptPiece &piece : m_editor->piecesToRun()) {
        if (piece.psqlCommand) {
            // Variables are handled in script order, as the jobs are queued.
            QString output;
            if (m_variables.apply(piece.sql, &output)) {
                if (!output.isEmpty())
                    m_panel->log(output);
            } else {
                m_panel->log(tr("Skipped psql command: %1").arg(preview(piece.sql)));
            }
            continue;
        }
        Job job;
        job.sql = substitute(piece.sql, job);
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

void EditorPage::explain(bool analyze)
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
    job.sql = prefix + substitute(pieces.front().sql, job);
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

void EditorPage::cancel()
{
    if (isRunning() && m_connection->cancel())
        m_panel->log(tr("Cancelling…"));
}

void EditorPage::start(std::deque<Job> jobs)
{
    m_editor->clearErrors();
    m_jobs = std::move(jobs);
    m_failed = false;
    startNext();
    Q_EMIT runningChanged(true);
}

void EditorPage::startNext()
{
    while (!m_jobs.empty()) {
        const Job &next = m_jobs.front();
        const bool runs = next.onFailure ? m_failed : (!m_failed || next.always);
        if (runs)
            break;
        m_jobs.pop_front();
    }
    if (m_jobs.empty()) {
        finishAll();
        return;
    }

    m_current = std::move(m_jobs.front());
    m_jobs.pop_front();
    m_rowsShown = false;
    m_rows = 0;
    m_commandTag.clear();
    m_editInfo.clear();
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

void EditorPage::onResult(const pg::Result &result)
{
    if (!m_current)
        return;
    const Job &job = *m_current;

    if (job.kind == Job::Kind::EditInfo) {
        m_editInfo.push_back(result); // Errors too: editTarget() then says it could not look up.
        return;
    }
    if (result.isError()) {
        if (m_limited && result.sqlState() == "57014")
            return; // Our own cancel at the row limit.
        m_failed = true;
        QString message = result.errorMessage();
        m_panel->log(message, true);
        const int position = result.errorPosition();
        if (position > job.prefixChars)
            markError(job, position - job.prefixChars, message);
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
    case Job::Kind::Dml:
        m_commandTag = result.commandTag();
        break;
    case Job::Kind::EditInfo:
        break;
    }
}

void EditorPage::onFinished()
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
        // Rows from one table can be edited; find out which, and its key.
        if (m_rowsShown && !m_failed) {
            m_lastQuery = job;
            lookUpEditTarget();
        }
    } else if (job.kind == Job::Kind::Explain && !m_failed) {
        m_panel->log(
            tr("Explained: %1 (%2 ms)").arg(preview(job.sql.mid(job.prefixChars))).arg(ms));
    } else if (job.kind == Job::Kind::EditInfo) {
        m_panel->results()->model()->setEditTarget(
            catalog::editTarget(m_panel->results()->model()->rows(), m_editInfo));
    } else if (job.kind == Job::Kind::Dml && !m_failed && !catalog::changedOneRow(m_commandTag)) {
        m_failed = true;
        m_panel->log(tr("%1 changed %2 instead of one row; nothing was saved.")
                         .arg(preview(job.sql), QString::fromUtf8(m_commandTag)),
                     true);
    }
    if (!job.message.isEmpty() && !m_failed)
        m_panel->log(job.message);
    startNext();
}

void EditorPage::lookUpEditTarget()
{
    QString reason;
    ResultModel *model = m_panel->results()->model();
    const catalog::Oid table = catalog::sourceTable(model->rows(), &reason);
    if (table == 0) {
        catalog::EditTarget none;
        none.reason = reason;
        model->setEditTarget(none);
        return;
    }
    // Next, before any further statement changes the rows on show.
    Job info;
    info.kind = Job::Kind::EditInfo;
    info.sql = catalog::editTargetQuery(table);
    m_jobs.push_front(std::move(info));
}

void EditorPage::saveChanges(bool confirm)
{
    ResultModel *model = m_panel->results()->model();
    if (!model->hasChanges() || !model->isEditable() || isRunning()
        || m_connection->state() != pg::Connection::State::Ready)
        return;
    const catalog::EditTarget &target = model->editTarget();
    const QByteArrayList statements = catalog::dmlStatements(target, model->changes());
    const PGTransactionStatusType status = m_connection->transactionStatus();
    if (status == PQTRANS_INERROR) {
        m_panel->log(tr("The current transaction has failed; roll it back before saving."), true);
        return;
    }
    const bool inTransaction = status == PQTRANS_INTRANS;
    if (confirm) {
        SaveChangesDialog dialog(target.schema + QLatin1Char('.') + target.table, statements,
                                 inTransaction, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
    }

    auto step = [](const QByteArray &sql, Job::Kind kind = Job::Kind::Silent) {
        Job j;
        j.kind = kind;
        j.sql = sql;
        return j;
    };
    // All of them or none: a transaction, or a savepoint in the open one.
    std::deque<Job> jobs;
    jobs.push_back(step(inTransaction ? "SAVEPOINT slonisko_save" : "BEGIN"));
    for (const QByteArray &statement : statements)
        jobs.push_back(step(statement, Job::Kind::Dml));
    Job done = step(inTransaction ? "RELEASE SAVEPOINT slonisko_save" : "COMMIT");
    done.message = inTransaction
        ? tr("Saved %n change(s) in the open transaction.", nullptr, int(statements.size()))
        : tr("Saved %n change(s).", nullptr, int(statements.size()));
    jobs.push_back(done);
    if (inTransaction) {
        Job back = step("ROLLBACK TO SAVEPOINT slonisko_save");
        back.onFailure = true;
        Job release = step("RELEASE SAVEPOINT slonisko_save");
        release.onFailure = true;
        jobs.push_back(back);
        jobs.push_back(release);
    } else {
        Job back = step("ROLLBACK");
        back.onFailure = true;
        jobs.push_back(back);
    }
    // Then show the rows as they are now.
    if (m_lastQuery)
        jobs.push_back(*m_lastQuery);
    start(std::move(jobs));
}

void EditorPage::finishAll()
{
    m_jobs.clear();
    m_current.reset();
    updateActions();
    updateStatus();
    Q_EMIT runningChanged(false);
}

void EditorPage::markError(const Job &job, int position, const QString &message)
{
    if (job.offset < 0)
        return;
    // A position in what was sent, back to where it is in the editor.
    const QByteArray sent = job.sql.mid(job.prefixChars);
    const qsizetype original
        = sql::PsqlVariables::originalOffset(bytesBefore(sent, position), job.replacements);
    m_editor->markError(job.offset + original, message);
}

QByteArray EditorPage::substitute(const QByteArray &sql, Job &job)
{
    QStringList unset;
    const QByteArray out = m_variables.substitute(sql, &job.replacements, &unset);
    for (const QString &name : unset)
        m_panel->log(tr("psql variable :%1 is not set; left as it is.").arg(name));
    return out;
}

QString EditorPage::preview(const QByteArray &sql)
{
    QString text = QString::fromUtf8(sql).simplified();
    if (text.size() > 60)
        text = text.left(60) + QChar(0x2026);
    return text;
}

} // namespace slonisko
