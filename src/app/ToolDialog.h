// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/ClientTools.h"

#include <QPointer>
#include <QWizard>
#include <QWizardPage>

#include <functional>
#include <map>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSettings;

namespace slonisko {

class Session;

// A wizard page whose wizard says when it is complete: the file page while
// what was chosen cannot run, the last one while the program runs.
class ToolPage : public QWizardPage
{
    Q_OBJECT

public:
    using QWizardPage::QWizardPage;

    bool isComplete() const override { return m_complete; }
    void setComplete(bool complete);

private:
    bool m_complete = true;
};

// What the backup and the restore wizards share: finding pg_dump and the
// like for the server's version, showing the command it will run, running
// it on the last page and showing what it writes there. A wizard, so that
// its choices fit a small screen a page at a time.
class ToolDialog : public QWizard
{
    Q_OBJECT

public:
    ~ToolDialog() override;

    bool isRunning() const;
    QString commandText() const;
    QString logText() const;
    // Why it cannot run, or a warning, as the first page shows it.
    QString problemText() const;
    QString statusText() const;
    // Whether the last page's button would run it.
    bool canRun() const;
    // How to ask before running; tests answer instead of a message box.
    void setConfirm(std::function<bool(const QString &question)> confirm)
    {
        m_confirm = std::move(confirm);
    }
    // Where to look for the programs first: tests point it at a version.
    static constexpr const char *DirectoryKey = "clientTools/directory";

Q_SIGNALS:
    void toolFinished(bool ok);

protected:
    struct Widgets
    {
        ToolPage *filePage = nullptr; // The first: the file, the program and what is wrong.
        QLabel *tool = nullptr;
        QPushButton *chooseTool = nullptr;
        QLabel *problem = nullptr;
        ToolPage *runPage = nullptr; // The last: the command, and running it.
        QLineEdit *command = nullptr;
        QPlainTextEdit *log = nullptr;
        QProgressBar *busy = nullptr;
        QLabel *status = nullptr;
    };

    // settings may be null: no remembered directory, then.
    ToolDialog(Session *session, const QString &database, QSettings *settings,
               QWidget *parent = nullptr);
    // After setupUi(); runText names the last page's Finish button.
    void setWidgets(const Widgets &widgets, const QString &runText);

    // The program to run: "pg_dump", "pg_restore" or "psql".
    virtual QString programName() const = 0;
    virtual QStringList arguments(const QByteArray &conninfo) const = 0;
    // Why it cannot run as chosen; empty if it can.
    virtual QString problem() const { return {}; }
    // The question to ask before running; empty to ask none.
    virtual QString question() const { return {}; }
    virtual QString doneMessage() const = 0;

    // "a, b" as {"a", "b"}, for the schema and table fields.
    static QStringList nameList(const QString &list);
    static void fillContents(QComboBox *contents);

    // When a choice changes: the command and whether it can run.
    void update();
    Session *session() const { return m_session; }
    QString database() const { return m_database; }
    // The database's name as the user knows it.
    QString databaseName() const;

private:
    std::optional<pg::ClientTool> tool(const QString &name);
    void chooseTool();
    void run();
    void onFinished(bool ok, const QString &message);
    static void setText(QLabel *label, const QString &text, bool error);
    // Finish runs the program rather than closing; Close closes.
    void accept() override;
    void reject() override;

    QPointer<Session> m_session;
    QString m_database;
    QSettings *m_settings;
    Widgets m_widgets;
    std::map<QString, std::optional<pg::ClientTool>> m_tools; // Found, by name.
    pg::ClientToolRun *m_run = nullptr;
    bool m_done = false; // Ran successfully since the last page was shown.
    std::function<bool(const QString &)> m_confirm;
};

} // namespace slonisko
