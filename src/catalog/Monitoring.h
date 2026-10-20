// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace slonisko::catalog {

// A prepared query about the server, shown as a table in the browser.
struct MonitoringQuery
{
    enum class Group { DbaTools, SystemInfo };

    Group group;
    QString id; // Stable, for settings and tests.
    QString title;
    QString description;
    // The SQL for a server version (PQserverVersion(), e.g. 180001).
    QByteArray (*sql)(int serverVersion);
    // Rows are server processes, by their "pid" column, which can be
    // cancelled or terminated.
    bool processes = false;
};

// In display order within each group.
const std::vector<MonitoringQuery> &monitoringQueries();
// Null for an id that is none of them.
const MonitoringQuery *monitoringQuery(const QString &id);

enum class ProcessAction {
    Cancel, // Stops the running query; the session stays.
    Terminate, // Ends the session, rolling back its transaction.
};
// One row per process: its pid, and whether the signal was sent ("done").
// False means it is no server process, or one this role may not signal.
QByteArray processActionSql(ProcessAction action, const std::vector<int> &pids);

} // namespace slonisko::catalog
