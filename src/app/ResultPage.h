// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "WorkspacePage.h"

#include <QByteArray>
#include <QPointer>

namespace slonisko {

class ResultView;
class Session;

namespace pg {
class Connection;
class QueryRunner;
}

// A query's rows on their own, like the DBA and System Info views. Runs the
// query on a connection of its own; Run Again refreshes it.
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

private:
    QPointer<Session> m_session;
    QString m_title;
    QByteArray m_sql;
    ResultView *m_view = nullptr;
    pg::Connection *m_connection = nullptr;
    pg::QueryRunner *m_runner = nullptr;
};

} // namespace slonisko
