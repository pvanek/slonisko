// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Health.h"

#include "pg/Result.h"

#include <algorithm>

namespace slonisko::quality {

namespace {

// Settings worth a look whatever their value; any setting waiting for a
// restart comes along too.
const char *const SettingNames[] = {
    "archive_mode",
    "autovacuum",
    "autovacuum_freeze_max_age",
    "autovacuum_multixact_freeze_max_age",
    "data_checksums",
    "effective_cache_size",
    "fsync",
    "full_page_writes",
    "idle_in_transaction_session_timeout",
    "maintenance_work_mem",
    "max_connections",
    "max_wal_size",
    "shared_buffers",
    "shared_preload_libraries",
    "synchronous_commit",
    "track_counts",
    "wal_level",
    "work_mem",
};

// A time as text the same whatever DateStyle the session has.
QByteArray timestamp(const QByteArray &expression)
{
    return "to_char(" + expression + ", 'YYYY-MM-DD HH24:MI')";
}

QString text(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QString() : QString::fromUtf8(result.value(row, column));
}

qint64 integer(const pg::Result &result, int row, int column)
{
    return result.value(row, column).toLongLong();
}

double number(const pg::Result &result, int row, int column)
{
    return result.value(row, column).toDouble();
}

bool flag(const pg::Result &result, int row, int column)
{
    return result.value(row, column) == "t";
}

void parseServer(const pg::Result &r, Health &health)
{
    if (r.rowCount() == 0)
        return;
    health.version = text(r, 0, 0);
    health.today = QDate::fromString(text(r, 0, 1), Qt::ISODate);
    health.startedAt = text(r, 0, 2);
    health.uptimeSeconds = integer(r, 0, 3);
    health.standby = flag(r, 0, 4);
    health.connections = int(integer(r, 0, 5));
    health.maxConnections = int(integer(r, 0, 6));
    health.reservedConnections = int(integer(r, 0, 7));
    health.monitor = flag(r, 0, 8);
    health.user = text(r, 0, 9);
}

void parseDatabase(const pg::Result &r, Health &health)
{
    if (r.rowCount() == 0)
        return;
    DatabaseHealth &d = health.database;
    d.name = text(r, 0, 0);
    d.size = integer(r, 0, 1);
    d.xidAge = integer(r, 0, 2);
    d.multixactAge = integer(r, 0, 3);
    d.commits = integer(r, 0, 4);
    d.rollbacks = integer(r, 0, 5);
    d.blocksRead = integer(r, 0, 6);
    d.blocksHit = integer(r, 0, 7);
    d.tempFiles = integer(r, 0, 8);
    d.tempBytes = integer(r, 0, 9);
    d.deadlocks = integer(r, 0, 10);
    d.checksumFailures = integer(r, 0, 11);
    d.statsReset = text(r, 0, 12);
    d.collationVersion = text(r, 0, 13);
    d.actualCollationVersion = text(r, 0, 14);
    d.statsAgeSeconds = integer(r, 0, 15);
}

void parsePart(Part part, const pg::Result &r, Health &health)
{
    const int rows = r.rowCount();
    if (part == Part::Server) {
        parseServer(r, health);
    } else if (part == Part::Settings) {
        for (int i = 0; i < rows; ++i)
            health.settings.push_back(
                {text(r, i, 0), text(r, i, 1), text(r, i, 2), text(r, i, 3), flag(r, i, 4)});
    } else if (part == Part::Database) {
        parseDatabase(r, health);
    } else if (part == Part::Databases) {
        for (int i = 0; i < rows; ++i)
            health.databases.push_back({text(r, i, 0), integer(r, i, 1), integer(r, i, 2)});
    } else if (part == Part::Sessions) {
        for (int i = 0; i < rows; ++i)
            health.sessions.push_back({int(integer(r, i, 0)), text(r, i, 1), text(r, i, 2),
                                       text(r, i, 3), text(r, i, 4), number(r, i, 5),
                                       number(r, i, 6), text(r, i, 7)});
    } else if (part == Part::Prepared) {
        for (int i = 0; i < rows; ++i)
            health.prepared.push_back(
                {text(r, i, 0), text(r, i, 1), text(r, i, 2), number(r, i, 3)});
    } else if (part == Part::Slots) {
        for (int i = 0; i < rows; ++i)
            health.slots.push_back({text(r, i, 0), text(r, i, 1), text(r, i, 2), flag(r, i, 3),
                                    r.isNull(i, 4) ? -1 : integer(r, i, 4), text(r, i, 5)});
    } else if (part == Part::Tables) {
        for (int i = 0; i < rows; ++i) {
            TableHealth t;
            t.oid = r.value(i, 0).toUInt();
            t.schema = text(r, i, 1);
            t.name = text(r, i, 2);
            t.seqScans = integer(r, i, 3);
            t.seqRowsRead = integer(r, i, 4);
            t.indexScans = integer(r, i, 5);
            t.liveRows = integer(r, i, 6);
            t.deadRows = integer(r, i, 7);
            t.modifiedSinceAnalyze = integer(r, i, 8);
            t.lastVacuum = text(r, i, 9);
            t.lastAnalyze = text(r, i, 10);
            t.analyzed = !t.lastAnalyze.isEmpty();
            t.size = integer(r, i, 11);
            t.indexesSize = integer(r, i, 12);
            t.xidAge = r.isNull(i, 13) ? -1 : integer(r, i, 13);
            health.tables.push_back(std::move(t));
        }
    } else if (part == Part::Indexes) {
        for (int i = 0; i < rows; ++i)
            health.indexes.push_back({text(r, i, 0), text(r, i, 1), text(r, i, 2), integer(r, i, 3),
                                      integer(r, i, 4), flag(r, i, 5)});
    } else if (part == Part::Sequences) {
        for (int i = 0; i < rows; ++i) {
            SequenceHealth s;
            s.schema = text(r, i, 0);
            s.name = text(r, i, 1);
            if (!r.isNull(i, 2))
                s.lastValue = integer(r, i, 2);
            s.minimum = integer(r, i, 3);
            s.maximum = integer(r, i, 4);
            s.increment = integer(r, i, 5);
            s.cycle = flag(r, i, 6);
            health.sequences.push_back(std::move(s));
        }
    } else if (part == Part::Extensions) {
        for (int i = 0; i < rows; ++i)
            health.extensionUpdates.push_back({text(r, i, 0), text(r, i, 1), text(r, i, 2)});
    } else if (part == Part::Privileges) {
        for (int i = 0; i < rows; ++i)
            health.publicCreateSchemas << text(r, i, 0);
    } else if (part == Part::Routines) {
        health.routinesChecked = true;
        for (int i = 0; i < rows; ++i)
            health.routineErrors.push_back(
                {text(r, i, 0), text(r, i, 1), int(integer(r, i, 2)), text(r, i, 3)});
    }
}

} // namespace

QString Setting::display() const
{
    bool ok = false;
    const qint64 amount = value.toLongLong(&ok);
    if (!ok || unit.isEmpty())
        return value;
    qint64 bytes = 0;
    if (unit == QLatin1String("8kB"))
        bytes = amount * 8192;
    else if (unit == QLatin1String("kB"))
        bytes = amount * 1024;
    else if (unit == QLatin1String("MB"))
        bytes = amount * 1024 * 1024;
    else if (unit == QLatin1String("B"))
        bytes = amount;
    else
        return value + unit; // 10s, 200ms, 5min
    if (amount < 0)
        return value; // -1 for "use another setting's".
    static const char *const units[] = {"B", "kB", "MB", "GB", "TB"};
    int u = 0;
    while (u < 4 && bytes % 1024 == 0 && bytes >= 1024) {
        bytes /= 1024;
        ++u;
    }
    return QString::number(bytes) + QLatin1String(units[u]);
}

const Setting *Health::setting(const QString &name) const
{
    const auto it = std::ranges::find(settings, name, &Setting::name);
    return it == settings.end() ? nullptr : &*it;
}

bool Health::has(Part part) const
{
    return versionNumber > 0
        && std::ranges::none_of(unavailable, [&](const Unavailable &u) { return u.part == part; });
}

QString partTitle(Part part)
{
    switch (part) {
    case Part::Server:
        return QStringLiteral("Server");
    case Part::Settings:
        return QStringLiteral("Settings");
    case Part::Database:
        return QStringLiteral("Database statistics");
    case Part::Databases:
        return QStringLiteral("Databases of the server");
    case Part::Sessions:
        return QStringLiteral("Sessions");
    case Part::Prepared:
        return QStringLiteral("Prepared transactions");
    case Part::Slots:
        return QStringLiteral("Replication slots");
    case Part::Tables:
        return QStringLiteral("Table statistics");
    case Part::Indexes:
        return QStringLiteral("Index statistics");
    case Part::Sequences:
        return QStringLiteral("Sequences");
    case Part::Extensions:
        return QStringLiteral("Extension versions");
    case Part::Privileges:
        return QStringLiteral("Schema privileges");
    case Part::Routines:
        return QStringLiteral("Routine bodies");
    }
    return {};
}

QByteArray versionQuery()
{
    return "SELECT current_setting('server_version_num')::int";
}

int parseVersion(const pg::Result &result)
{
    return result.rowCount() > 0 ? result.value(0, 0).toInt() : 0;
}

std::vector<HealthQuery> healthQueries(int version, const docgen::SchemaFilter &filter,
                                       const QString &plpgsqlCheck)
{
    const QByteArray schemas = docgen::schemaCondition(filter);
    std::vector<HealthQuery> queries;

    // Since 16, reserved_connections keeps some for pg_use_reserved_connections.
    const QByteArray reserved = version >= 160000
        ? "current_setting('superuser_reserved_connections')::int "
          "+ current_setting('reserved_connections')::int"
        : "current_setting('superuser_reserved_connections')::int";
    queries.push_back(
        {Part::Server,
         "SELECT current_setting('server_version'), to_char(current_date, 'YYYY-MM-DD'), "
             + timestamp("pg_postmaster_start_time()")
             + ", extract(epoch FROM now() - pg_postmaster_start_time())::bigint, "
               "pg_is_in_recovery(), "
               "(SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend'), "
               "current_setting('max_connections')::int, "
             + reserved
             + ", r.rolsuper OR pg_has_role(r.oid, 'pg_monitor', 'MEMBER'), current_user "
               "FROM pg_roles r WHERE r.rolname = current_user"});

    QByteArray names;
    for (const char *name : SettingNames)
        names += (names.isEmpty() ? "'" : ", '") + QByteArray(name) + '\'';
    queries.push_back({Part::Settings,
                       "SELECT name, setting, coalesce(unit, ''), source, pending_restart "
                       "FROM pg_settings WHERE name IN ("
                           + names + ") OR pending_restart ORDER BY name"});

    queries.push_back(
        {Part::Database,
         "SELECT d.datname, pg_database_size(d.oid), age(d.datfrozenxid), "
         "mxid_age(d.datminmxid), s.xact_commit, s.xact_rollback, s.blks_read, s.blks_hit, "
         "s.temp_files, s.temp_bytes, s.deadlocks, "
             + QByteArray(version >= 120000 ? "coalesce(s.checksum_failures, 0), " : "0, ")
             + timestamp("s.stats_reset") + ", "
             + QByteArray(version >= 150000
                              ? "d.datcollversion, pg_database_collation_actual_version(d.oid)"
                              : "NULL, NULL")
             + ", extract(epoch FROM now() - coalesce(s.stats_reset, "
               "pg_postmaster_start_time()))::bigint "
               "FROM pg_database d LEFT JOIN pg_stat_database s ON s.datid = d.oid "
               "WHERE d.datname = current_database()"});

    queries.push_back({Part::Databases,
                       "SELECT datname, age(datfrozenxid), mxid_age(datminmxid) "
                       "FROM pg_database ORDER BY 2 DESC"});

    // Every open transaction; the analysis decides which are too old.
    queries.push_back(
        {Part::Sessions,
         "SELECT pid, usename, datname, application_name, state, "
         "extract(epoch FROM now() - xact_start), extract(epoch FROM now() - state_change), "
         "left(query, 300) FROM pg_stat_activity "
         "WHERE backend_type = 'client backend' AND pid <> pg_backend_pid() "
         "AND xact_start IS NOT NULL ORDER BY xact_start LIMIT 1000"});

    queries.push_back({Part::Prepared,
                       "SELECT gid, owner, database, extract(epoch FROM now() - prepared) "
                       "FROM pg_prepared_xacts ORDER BY prepared"});

    // pg_current_wal_lsn() fails on a standby.
    queries.push_back({Part::Slots,
                       "SELECT slot_name, slot_type, database, active, "
                       "CASE WHEN NOT pg_is_in_recovery() AND restart_lsn IS NOT NULL "
                       "THEN pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)::bigint END, "
                           + QByteArray(version >= 130000 ? "wal_status " : "NULL ")
                           + "FROM pg_replication_slots ORDER BY slot_name"});

    queries.push_back(
        {Part::Tables,
         "SELECT s.relid, s.schemaname, s.relname, coalesce(s.seq_scan, 0), "
         "coalesce(s.seq_tup_read, 0), coalesce(s.idx_scan, 0), coalesce(s.n_live_tup, 0), "
         "coalesce(s.n_dead_tup, 0), coalesce(s.n_mod_since_analyze, 0), "
             + timestamp("greatest(s.last_vacuum, s.last_autovacuum)") + ", "
             + timestamp("greatest(s.last_analyze, s.last_autoanalyze)")
             + ", pg_table_size(s.relid), pg_indexes_size(s.relid), "
               "CASE WHEN c.relkind IN ('r', 'm') THEN age(c.relfrozenxid) END "
               "FROM pg_stat_user_tables s JOIN pg_class c ON c.oid = s.relid "
               "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE "
             + schemas + " ORDER BY s.schemaname, s.relname"});

    // An index of a partition belongs to its parent's index and cannot be
    // dropped alone.
    queries.push_back(
        {Part::Indexes,
         "SELECT s.schemaname, s.relname, s.indexrelname, s.idx_scan, "
         "pg_relation_size(s.indexrelid), i.indisunique OR EXISTS (SELECT 1 FROM pg_constraint "
         "k WHERE k.conindid = s.indexrelid AND k.conrelid = s.relid "
         "AND k.contype IN ('p', 'u', 'x')) "
         "FROM pg_stat_user_indexes s JOIN pg_index i ON i.indexrelid = s.indexrelid "
         "JOIN pg_class c ON c.oid = s.relid JOIN pg_namespace n ON n.oid = c.relnamespace "
         "WHERE NOT EXISTS (SELECT 1 FROM pg_inherits h WHERE h.inhrelid = s.indexrelid) AND "
             + schemas + " ORDER BY 1, 2, 3"});

    queries.push_back({Part::Sequences,
                       "SELECT s.schemaname, s.sequencename, s.last_value, s.min_value, "
                       "s.max_value, s.increment_by, s.cycle FROM pg_sequences s "
                       "JOIN pg_namespace n ON n.nspname = s.schemaname WHERE "
                           + schemas + " ORDER BY 1, 2"});

    queries.push_back({Part::Extensions,
                       "SELECT e.extname, e.extversion, a.default_version FROM pg_extension e "
                       "JOIN pg_available_extensions a ON a.name = e.extname "
                       "WHERE a.default_version IS DISTINCT FROM e.extversion ORDER BY 1"});

    // Grantee 0 is PUBLIC; no ACL at all means the default, the owner's.
    queries.push_back({Part::Privileges,
                       "SELECT n.nspname FROM pg_namespace n WHERE " + schemas
                           + " AND EXISTS (SELECT 1 FROM "
                             "aclexplode(coalesce(n.nspacl, acldefault('n', n.nspowner))) a "
                             "WHERE a.grantee = 0 AND a.privilege_type = 'CREATE') ORDER BY 1"});

    // A trigger function is checked against a table it is a trigger on; one
    // on none cannot be, and neither can an event trigger's.
    if (!plpgsqlCheck.isEmpty())
        queries.push_back(
            {Part::Routines,
             "SELECT n.nspname, p.proname || '(' || pg_get_function_identity_arguments(p.oid) "
             "|| ')', coalesce(r.lineno, 0), r.message "
             "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
             "JOIN pg_language l ON l.oid = p.prolang "
             "CROSS JOIN LATERAL (SELECT (SELECT t.tgrelid FROM pg_trigger t "
             "WHERE t.tgfoid = p.oid LIMIT 1) AS relid) t "
             "CROSS JOIN LATERAL "
                 + docgen::quoteIdentifier(plpgsqlCheck).toUtf8()
                 + ".plpgsql_check_function_tb(p.oid, coalesce(t.relid, 0)) r "
                   "WHERE l.lanname = 'plpgsql' AND p.prorettype <> 'event_trigger'::regtype "
                   "AND (p.prorettype <> 'trigger'::regtype OR t.relid IS NOT NULL) "
                   "AND r.level = 'error' AND "
                 + schemas
                 + " AND NOT EXISTS (SELECT 1 FROM pg_depend e WHERE e.classid = "
                   "'pg_proc'::regclass AND e.objid = p.oid AND e.deptype = 'e') "
                   "ORDER BY 1, 2, 3"});
    return queries;
}

Health parseHealth(int serverVersion, const std::vector<HealthQuery> &queries,
                   const std::vector<pg::Result> &results, const QStringList &errors)
{
    Health health;
    health.versionNumber = serverVersion;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const QString error = errors.value(qsizetype(i));
        const bool ok = error.isEmpty() && i < results.size() && !results[i].isNull()
            && results[i].status() == PGRES_TUPLES_OK;
        if (!ok) {
            health.unavailable.push_back(
                {queries[i].part, error.isEmpty() ? QStringLiteral("no result") : error.trimmed()});
            continue;
        }
        parsePart(queries[i].part, results[i], health);
    }
    return health;
}

} // namespace slonisko::quality
