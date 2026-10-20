// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Monitoring.h"
#include "WorkspacePage.h"

#include <QByteArray>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <vector>

class QAction;
class QComboBox;
class QToolButton;

namespace slonisko {

class ResultView;
class Session;

namespace pg {
class Connection;
class QueryRunner;
}

// A query's rows on their own, like the DBA and System Info views. Runs the
// query on a connection of its own; Run Again refreshes it, and so does the
// timer when auto-refresh is on.
class ResultPage : public WorkspacePage
{
    Q_OBJECT

public:
    ResultPage(Session *session, const QString &title, const QByteArray &sql,
               QWidget *parent = nullptr);

    QString title() const override { return m_title; }
    QColor color() const override;

    Session *session() const { return m_session; }
    QByteArray sql() const { return m_sql; }
    ResultView *results() const { return m_view; }
    // Its own, so it neither waits for nor holds up the object browser.
    pg::Connection *connection() const { return m_connection; }
    // Runs the query again.
    void refresh();

    // Seconds between refreshes; 0 for none. Only while the page is shown.
    void setAutoRefresh(int seconds);
    int autoRefresh() const;
    QComboBox *autoRefreshBox() const { return m_autoRefresh; }

    // The rows are server processes, by their "pid" column: offers to
    // cancel their queries or terminate them.
    void setProcessActions(bool on);
    QAction *cancelAction() const { return m_cancel; }
    QAction *terminateAction() const { return m_terminate; }
    // The pids of the selected rows.
    std::vector<int> selectedPids() const;
    // Cancels or terminates the selected processes, after asking.
    void signalSelected(catalog::ProcessAction action);
    // Asks before signalling; tests answer instead of a message box.
    using Confirm = std::function<bool(const QString &question)>;
    void setConfirm(Confirm confirm) { m_confirm = std::move(confirm); }

protected:
    void showEvent(QShowEvent *event) override;

private:
    void openConnection();
    void updateProcessActions();

    QPointer<Session> m_session;
    QString m_title;
    QByteArray m_sql;
    ResultView *m_view = nullptr;
    pg::Connection *m_connection = nullptr;
    pg::QueryRunner *m_runner = nullptr;
    QComboBox *m_autoRefresh = nullptr;
    QTimer m_timer;
    bool m_processes = false;
    QAction *m_cancel = nullptr;
    QAction *m_terminate = nullptr;
    QList<QToolButton *> m_processButtons;
    Confirm m_confirm;
};

} // namespace slonisko
