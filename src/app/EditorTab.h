// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Connection.h"
#include "sql/PsqlVariables.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <deque>
#include <optional>

class QAction;
class QComboBox;
class QLabel;

namespace slonisko {

class ConnectionBrowser;
class ResultPanel;
class Session;
class SqlEditor;

// A SQL editor with a connection of its own to one of the connected
// sessions. Its results go to a ResultPanel, which the main window shows
// below the editors.
class EditorTab : public QWidget
{
    Q_OBJECT

public:
    EditorTab(ConnectionBrowser *browser, const QString &name, QWidget *parent = nullptr);
    ~EditorTab() override;

    SqlEditor *editor() const { return m_editor; }
    ResultPanel *resultPanel() const { return m_panel; }
    pg::Connection *connection() const { return m_connection; }
    Session *session() const { return m_session; }
    QString database() const { return m_database; }
    // The file name (or the tab's name), * when modified, and the connection.
    QString title() const;
    // The connection's color, if its profile has one.
    QColor color() const;

    // Files. Text is read and written as UTF-8, with the file's line endings.
    QString filePath() const { return m_filePath; }
    bool isModified() const;
    // Whether nothing was typed or opened yet, so opening a file can reuse the tab.
    bool isBlank() const;
    bool openFile(const QString &path, QString *error = nullptr);
    bool saveFile(const QString &path, QString *error = nullptr);
    // Save to the file, or ask where if there is none. False if cancelled or failed.
    bool save();
    bool saveAs();
    // Before closing: asks to save changes. False to keep the tab open.
    bool maybeSave();

    // Connects the editor to a database of a session; null disconnects.
    void setSession(Session *session, const QString &database = {});

    void run();
    void explain(bool analyze);
    void cancel();
    // Writes the result's edits back to its table; confirm shows the SQL first.
    void saveChanges(bool confirm = true);
    bool isRunning() const { return m_current.has_value(); }

    // Stops a query once it returned this many rows.
    void setRowLimit(int rows) { m_rowLimit = rows; }
    // psql variables set by \set in this editor, like in a psql session.
    const sql::PsqlVariables &variables() const { return m_variables; }

Q_SIGNALS:
    void titleChanged();
    void runningChanged(bool running);

private:
    struct Job
    {
        enum class Kind {
            Statement,
            Explain,
            Silent,
            EditInfo, // Looks up the table a result can be edited in.
            Dml, // Saves one edit; must change exactly one row.
        };
        Kind kind = Kind::Statement;
        QByteArray sql;
        std::optional<QByteArray> copyData;
        qsizetype offset = -1; // In the document, to mark errors; -1 for none.
        int prefixChars = 0; // Characters added before the user's text, like EXPLAIN.
        // Where psql variables were put into the user's text.
        std::vector<sql::PsqlVariables::Replacement> replacements;
        bool always = false; // Runs even after an error, like ROLLBACK.
        bool onFailure = false; // Runs only after an error.
        bool analyze = false;
        QString message; // Logged when it succeeds.
    };

    void updateSessions();
    void updateTargetColor();
    void updateActions();
    void updateStatus();
    void start(std::deque<Job> jobs);
    void startNext();
    void onResult(const pg::Result &result);
    void onFinished();
    void onConnectionState(pg::Connection::State state);
    void finishAll();
    void lookUpEditTarget();
    // Replaces psql variables in a statement, noting the ones not set.
    QByteArray substitute(const QByteArray &sql, Job &job);
    void markError(const Job &job, int position, const QString &message);
    static QString preview(const QByteArray &sql);

    ConnectionBrowser *m_browser = nullptr;
    QString m_name;
    QString m_filePath;
    SqlEditor *m_editor = nullptr;
    ResultPanel *m_panel = nullptr;
    QComboBox *m_target = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_run = nullptr;
    QAction *m_explain = nullptr;
    QAction *m_explainAnalyze = nullptr;
    QAction *m_cancel = nullptr;

    QPointer<Session> m_session;
    QString m_database;
    pg::Connection *m_connection = nullptr;

    std::deque<Job> m_jobs;
    std::optional<Job> m_current;
    QElapsedTimer m_timer;
    bool m_failed = false; // A job failed: skip the rest but the "always" ones.
    bool m_rowsShown = false; // The current job's rows went to the result view.
    qint64 m_rows = 0;
    QByteArray m_commandTag;
    bool m_limited = false; // Cancelled at the row limit.
    int m_rowLimit = 100'000;
    sql::PsqlVariables m_variables;
    std::optional<Job>
        m_lastQuery; // The statement whose rows are shown, to run again after saving.
    std::vector<pg::Result> m_editInfo;
};

} // namespace slonisko
