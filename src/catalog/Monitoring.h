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
};

// In display order within each group.
const std::vector<MonitoringQuery> &monitoringQueries();

} // namespace slonisko::catalog
