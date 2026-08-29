// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/QueryRunner.h"

namespace slonisko::pg {

QueryRunner::QueryRunner(Connection *connection, QObject *parent)
    : QObject(parent), m_connection(connection)
{
    connect(connection, &Connection::resultReady, this,
            [this](const Result &r) { m_results.push_back(r); });
    connect(connection, &Connection::queryFinished, this, &QueryRunner::onFinished);
    connect(connection, &Connection::stateChanged, this, &QueryRunner::onStateChanged);
}

void QueryRunner::run(const QByteArray &sql, QObject *context, Callback callback)
{
    m_queue.push_back({sql, context, std::move(callback)});
    if (m_connection && m_connection->state() == Connection::State::Failed) {
        // Report it later, like any other outcome.
        QMetaObject::invokeMethod(
            this, [this] { failAll(m_connection ? m_connection->errorMessage() : QString()); },
            Qt::QueuedConnection);
        return;
    }
    startNext();
}

void QueryRunner::startNext()
{
    if (m_current || m_queue.empty() || !m_connection
        || m_connection->state() != Connection::State::Ready)
        return;

    m_current = std::move(m_queue.front());
    m_queue.pop_front();
    m_results.clear();
    if (!m_connection->execute(m_current->sql))
        failAll(m_connection->errorMessage());
}

void QueryRunner::onFinished()
{
    if (!m_current)
        return; // Someone else's query on the same connection.

    QueryOutcome outcome;
    outcome.results = std::move(m_results);
    m_results.clear();
    for (const Result &r : outcome.results) {
        if (r.isError()) {
            outcome.error = r.errorMessage();
            outcome.sqlState = r.sqlState();
            break;
        }
    }

    Job job = std::move(*m_current);
    m_current.reset();
    const QPointer<QueryRunner> guard(this);
    complete(job, outcome);
    if (guard)
        startNext();
}

void QueryRunner::onStateChanged(Connection::State state)
{
    switch (state) {
    case Connection::State::Ready:
        startNext();
        break;
    case Connection::State::Failed:
        failAll(m_connection->errorMessage());
        break;
    case Connection::State::Disconnected:
        failAll(tr("Disconnected"));
        break;
    default:
        break;
    }
}

void QueryRunner::failAll(const QString &message)
{
    std::deque<Job> jobs;
    if (m_current)
        jobs.push_back(std::move(*m_current));
    m_current.reset();
    m_results.clear();
    for (Job &job : m_queue)
        jobs.push_back(std::move(job));
    m_queue.clear();

    QueryOutcome outcome;
    outcome.error = message.isEmpty() ? tr("Not connected") : message;
    for (Job &job : jobs)
        complete(job, outcome); // Contexts guard the callbacks, not the runner.
}

void QueryRunner::complete(Job &job, const QueryOutcome &outcome)
{
    if (job.context && job.callback)
        job.callback(outcome);
}

} // namespace slonisko::pg
