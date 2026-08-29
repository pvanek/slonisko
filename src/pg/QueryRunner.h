// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Connection.h"
#include "pg/Result.h"

#include <QPointer>

#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace slonisko::pg {

struct QueryOutcome
{
    std::vector<Result> results;
    // The error that ended the query, or why the connection failed. Empty on success.
    QString error;
    QByteArray sqlState;

    bool ok() const { return error.isEmpty(); }
};

// Runs queries on one connection one after another, for callers that each
// want to run a query and get a callback, like the object browser. Queries
// wait while the connection is still connecting.
class QueryRunner : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(const QueryOutcome &)>;

    explicit QueryRunner(Connection *connection, QObject *parent = nullptr);

    Connection *connection() const { return m_connection; }

    // Queues sql. callback runs once it is done, or failed because the
    // connection did, unless context has been deleted by then. Callbacks may
    // queue more queries or delete the runner.
    void run(const QByteArray &sql, QObject *context, Callback callback);

    // Queued and running queries.
    int pending() const { return int(m_queue.size()) + (m_current ? 1 : 0); }

private:
    struct Job
    {
        QByteArray sql;
        QPointer<QObject> context;
        Callback callback;
    };

    void startNext();
    void onFinished();
    void onStateChanged(Connection::State state);
    void failAll(const QString &message);
    static void complete(Job &job, const QueryOutcome &outcome);

    QPointer<Connection> m_connection;
    std::deque<Job> m_queue;
    std::optional<Job> m_current;
    std::vector<Result> m_results;
};

} // namespace slonisko::pg
