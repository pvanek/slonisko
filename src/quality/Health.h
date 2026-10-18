// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Model.h"

#include <QByteArray>
#include <QDate>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::quality {

// What the server and its statistics say about how a database is doing,
// beside what its catalog says about how it is made. Plain data, like
// docgen::Model, so the report can be tested on health made by hand.
//
// Each part comes from a query of its own, and a query the account may not
// run only leaves its part out: unavailable names it, with the reason.

struct Setting
{
    QString name;
    QString value; // pg_settings.setting, in its unit.
    QString unit; // "8kB", "ms", ... or empty.
    QString source;
    bool pendingRestart = false;

    // The value with its unit, as postgresql.conf would have it: "128MB".
    QString display() const;
};

struct DatabaseHealth
{
    QString name;
    qint64 size = 0;
    qint64 xidAge = 0; // age(datfrozenxid)
    qint64 multixactAge = 0;
    qint64 commits = 0;
    qint64 rollbacks = 0;
    qint64 blocksRead = 0;
    qint64 blocksHit = 0;
    qint64 tempFiles = 0;
    qint64 tempBytes = 0;
    qint64 deadlocks = 0;
    qint64 checksumFailures = 0;
    QString statsReset; // Empty: since the server started.
    qint64 statsAgeSeconds = 0; // Since the reset, or else the start.
    QString collationVersion; // datcollversion, 15+.
    QString actualCollationVersion; // What the operating system has now.
};

// Another database of the server, for transaction ID wraparound, which
// stops the whole server whichever database causes it.
struct ServerDatabase
{
    QString name;
    qint64 xidAge = 0;
    qint64 multixactAge = 0;
};

// A session with a transaction open for a while.
struct Session
{
    int pid = 0;
    QString user;
    QString database;
    QString application;
    QString state;
    double transactionSeconds = 0;
    double stateSeconds = 0; // Since the state last changed.
    QString query;
};

struct PreparedTransaction
{
    QString gid;
    QString owner;
    QString database;
    double ageSeconds = 0;
};

struct ReplicationSlot
{
    QString name;
    QString type; // physical, logical
    QString database;
    bool active = false;
    qint64 retainedWal = -1; // Bytes; -1 on a standby.
    QString walStatus; // reserved, extended, unreserved, lost (13+).
};

struct TableHealth
{
    docgen::Oid oid = 0;
    QString schema;
    QString name;
    qint64 seqScans = 0;
    qint64 seqRowsRead = 0;
    qint64 indexScans = 0;
    qint64 liveRows = 0;
    qint64 deadRows = 0;
    qint64 modifiedSinceAnalyze = 0;
    QString lastVacuum; // The later of manual and auto; empty for never.
    QString lastAnalyze;
    bool analyzed = false; // Ever, by hand or by autovacuum.
    qint64 size = 0; // pg_table_size(): data and TOAST, no indexes.
    qint64 indexesSize = 0;
    qint64 xidAge = -1; // age(relfrozenxid); -1 for a partitioned table.
};

struct IndexHealth
{
    QString schema;
    QString table;
    QString name;
    qint64 scans = 0;
    qint64 size = 0;
    // Unique or enforcing a constraint: needed whether it is read or not.
    bool enforcing = false;
};

struct SequenceHealth
{
    QString schema;
    QString name;
    std::optional<qint64> lastValue; // None before the first nextval().
    qint64 minimum = 1;
    qint64 maximum = 0;
    qint64 increment = 1;
    bool cycle = false;
};

struct ExtensionUpdate
{
    QString name;
    QString installed;
    QString available;
};

// An error plpgsql_check finds in a routine's body.
struct RoutineError
{
    QString schema;
    QString routine; // Its signature.
    int line = 0;
    QString message;
};

// The parts of Health, each read by a query of its own.
enum class Part {
    Server,
    Settings,
    Database,
    Databases,
    Sessions,
    Prepared,
    Slots,
    Tables,
    Indexes,
    Sequences,
    Extensions,
    Privileges,
    Routines,
};

QString partTitle(Part part);

struct Unavailable
{
    Part part;
    QString error;
};

struct Health
{
    int versionNumber = 0; // server_version_num
    QString version;
    QDate today; // The server's.
    QString startedAt;
    qint64 uptimeSeconds = 0;
    bool standby = false;
    bool monitor = false; // Superuser or pg_monitor: sees everything.
    QString user;
    int connections = 0;
    int maxConnections = 0;
    int reservedConnections = 0;

    std::vector<Setting> settings;
    DatabaseHealth database;
    std::vector<ServerDatabase> databases;
    std::vector<Session> sessions;
    std::vector<PreparedTransaction> prepared;
    std::vector<ReplicationSlot> slots;
    std::vector<TableHealth> tables;
    std::vector<IndexHealth> indexes;
    std::vector<SequenceHealth> sequences;
    std::vector<ExtensionUpdate> extensionUpdates;
    QStringList publicCreateSchemas; // Schemas anyone may create objects in.
    bool routinesChecked = false; // plpgsql_check ran.
    std::vector<RoutineError> routineErrors;
    std::vector<Unavailable> unavailable;

    const Setting *setting(const QString &name) const;
    // Whether the query reading the part ran.
    bool has(Part part) const;
};

// What to look at; the version decides how the queries are spelt.
struct HealthQuery
{
    Part part;
    QByteArray sql;
};

QByteArray versionQuery();
int parseVersion(const pg::Result &result);

// The queries after the version one, to run in order, each on its own: one
// failing does not stop the others. plpgsqlCheck is the schema the
// plpgsql_check extension is in, or empty when it is not installed.
std::vector<HealthQuery> healthQueries(int serverVersion, const docgen::SchemaFilter &filter,
                                       const QString &plpgsqlCheck = {});
// The results in the order of the queries, a failed query's an empty one
// beside its error.
Health parseHealth(int serverVersion, const std::vector<HealthQuery> &queries,
                   const std::vector<pg::Result> &results, const QStringList &errors);

} // namespace slonisko::quality
