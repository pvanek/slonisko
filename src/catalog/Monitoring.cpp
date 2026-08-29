// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Monitoring.h"

namespace slonisko::catalog {

namespace {

using Group = MonitoringQuery::Group;

QByteArray sessions(int)
{
    return "SELECT pid, usename AS user, datname AS database, application_name, client_addr, "
           "state, wait_event_type, wait_event, backend_start, xact_start, query_start, "
           "now() - query_start AS running_for, backend_type, query "
           "FROM pg_stat_activity WHERE pid <> pg_backend_pid() "
           "ORDER BY backend_type <> 'client backend', state = 'idle', query_start";
}

QByteArray locks(int)
{
    return "SELECT l.pid, a.usename AS user, a.datname AS database, l.locktype, l.mode, "
           "l.granted, l.relation::regclass AS relation, l.transactionid, l.virtualxid, "
           "pg_blocking_pids(l.pid) AS blocked_by, now() - a.query_start AS running_for, a.query "
           "FROM pg_locks l LEFT JOIN pg_stat_activity a ON a.pid = l.pid "
           "WHERE l.pid IS DISTINCT FROM pg_backend_pid() ORDER BY l.granted, l.pid";
}

QByteArray blocking(int)
{
    return "SELECT a.pid, a.usename AS user, a.datname AS database, "
           "pg_blocking_pids(a.pid) AS blocked_by, now() - a.query_start AS waiting_for, "
           "a.wait_event_type, a.wait_event, a.query "
           "FROM pg_stat_activity a WHERE cardinality(pg_blocking_pids(a.pid)) > 0 "
           "ORDER BY a.query_start";
}

QByteArray overview(int)
{
    return "SELECT version() AS version, pg_postmaster_start_time() AS started, "
           "date_trunc('second', now() - pg_postmaster_start_time()) AS uptime, "
           "pg_is_in_recovery() AS standby, current_setting('server_encoding') AS encoding, "
           "(SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend') "
           "AS connections, current_setting('max_connections')::int AS max_connections";
}

QByteArray changedSettings(int)
{
    return "SELECT name, setting, unit, source, short_desc AS description FROM pg_settings "
           "WHERE source NOT IN ('default', 'override') ORDER BY name";
}

QByteArray databaseSizes(int)
{
    return "SELECT d.datname AS database, pg_size_pretty(s.bytes) AS size, s.bytes "
           "FROM pg_database d CROSS JOIN LATERAL (SELECT CASE WHEN "
           "has_database_privilege(d.oid, 'CONNECT') THEN pg_database_size(d.oid) END AS bytes) s "
           "ORDER BY s.bytes DESC NULLS LAST";
}

QByteArray largestTables(int)
{
    return "SELECT n.nspname AS schema, c.relname AS table, "
           "pg_size_pretty(pg_total_relation_size(c.oid)) AS total, "
           "pg_size_pretty(pg_relation_size(c.oid)) AS data, "
           "pg_size_pretty(pg_indexes_size(c.oid)) AS indexes, "
           "c.reltuples::bigint AS estimated_rows "
           "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
           "WHERE c.relkind IN ('r', 'm') AND n.nspname NOT IN ('pg_catalog', "
           "'information_schema') "
           "AND n.nspname !~ '^pg_toast' "
           "ORDER BY pg_total_relation_size(c.oid) DESC LIMIT 100";
}

QByteArray cacheHitRatio(int)
{
    return "SELECT datname AS database, blks_hit, blks_read, "
           "round(100.0 * blks_hit / nullif(blks_hit + blks_read, 0), 2) AS hit_percent "
           "FROM pg_stat_database WHERE datname IS NOT NULL ORDER BY datname";
}

QByteArray indexUsage(int)
{
    return "SELECT schemaname AS schema, relname AS table, indexrelname AS index, idx_scan AS "
           "scans, "
           "pg_size_pretty(pg_relation_size(indexrelid)) AS size "
           "FROM pg_stat_user_indexes ORDER BY idx_scan, pg_relation_size(indexrelid) DESC LIMIT "
           "100";
}

QByteArray vacuumStatus(int)
{
    return "SELECT schemaname AS schema, relname AS table, n_live_tup AS live_rows, "
           "n_dead_tup AS dead_rows, "
           "round(100.0 * n_dead_tup / nullif(n_live_tup + n_dead_tup, 0), 1) AS dead_percent, "
           "last_vacuum, last_autovacuum, last_analyze, last_autoanalyze "
           "FROM pg_stat_user_tables ORDER BY n_dead_tup DESC LIMIT 100";
}

QByteArray vacuumProgress(int)
{
    return "SELECT p.pid, p.datname AS database, p.relid::regclass AS table, p.phase, "
           "p.heap_blks_total, p.heap_blks_scanned, p.heap_blks_vacuumed, "
           "now() - a.xact_start AS running_for "
           "FROM pg_stat_progress_vacuum p LEFT JOIN pg_stat_activity a ON a.pid = p.pid";
}

QByteArray longRunning(int)
{
    return "SELECT pid, usename AS user, datname AS database, state, "
           "now() - query_start AS running_for, now() - xact_start AS in_transaction_for, "
           "wait_event_type, wait_event, query FROM pg_stat_activity "
           "WHERE state <> 'idle' AND pid <> pg_backend_pid() "
           "AND least(query_start, xact_start) < now() - interval '1 minute' "
           "ORDER BY least(query_start, xact_start)";
}

QByteArray replication(int)
{
    return "SELECT pid, usename AS user, application_name, client_addr, state, sync_state, "
           "sent_lsn, write_lsn, flush_lsn, replay_lsn, write_lag, flush_lag, replay_lag "
           "FROM pg_stat_replication ORDER BY application_name";
}

QByteArray replicationSlots(int)
{
    // pg_current_wal_lsn() fails on a standby.
    return "SELECT slot_name, plugin, slot_type, database, active, restart_lsn, "
           "CASE WHEN NOT pg_is_in_recovery() THEN "
           "pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)) END AS retained_wal "
           "FROM pg_replication_slots ORDER BY slot_name";
}

QByteArray checkpoints(int version)
{
    if (version >= 170000)
        return "SELECT * FROM pg_stat_checkpointer";
    return "SELECT checkpoints_timed, checkpoints_req, checkpoint_write_time, "
           "checkpoint_sync_time, buffers_checkpoint, buffers_clean, buffers_backend, stats_reset "
           "FROM pg_stat_bgwriter";
}

QByteArray wal(int version)
{
    if (version >= 140000)
        return "SELECT * FROM pg_stat_wal";
    return "SELECT 'Requires PostgreSQL 14 or later' AS message";
}

QByteArray io(int version)
{
    if (version >= 160000)
        return "SELECT * FROM pg_stat_io WHERE reads > 0 OR writes > 0 OR extends > 0 "
               "ORDER BY backend_type, object, context";
    return "SELECT 'Requires PostgreSQL 16 or later' AS message";
}

QByteArray topStatements(int version)
{
    if (version < 130000)
        return "SELECT 'Requires PostgreSQL 13 or later' AS message";
    return "SELECT round(total_exec_time::numeric, 1) AS total_ms, calls, "
           "round(mean_exec_time::numeric, 2) AS mean_ms, rows, "
           "round(100.0 * shared_blks_hit / nullif(shared_blks_hit + shared_blks_read, 0), 1) "
           "AS hit_percent, query FROM pg_stat_statements ORDER BY total_exec_time DESC LIMIT 50";
}

} // namespace

const std::vector<MonitoringQuery> &monitoringQueries()
{
    static const std::vector<MonitoringQuery> queries {
        {Group::DbaTools, QStringLiteral("sessions"), QStringLiteral("Sessions"),
         QStringLiteral("All server processes and what they are doing"), sessions},
        {Group::DbaTools, QStringLiteral("locks"), QStringLiteral("Locks"),
         QStringLiteral("Held and awaited locks, waiting ones first"), locks},
        {Group::DbaTools, QStringLiteral("blocking"), QStringLiteral("Blocked Sessions"),
         QStringLiteral("Sessions waiting for a lock and who holds it"), blocking},
        {Group::DbaTools, QStringLiteral("long-running"), QStringLiteral("Long-Running Queries"),
         QStringLiteral("Queries and transactions active for over a minute"), longRunning},

        {Group::SystemInfo, QStringLiteral("overview"), QStringLiteral("Server Overview"),
         QStringLiteral("Version, uptime and connections"), overview},
        {Group::SystemInfo, QStringLiteral("settings"), QStringLiteral("Changed Settings"),
         QStringLiteral("Settings that differ from the built-in defaults"), changedSettings},
        {Group::SystemInfo, QStringLiteral("database-sizes"), QStringLiteral("Database Sizes"),
         QStringLiteral("Size of each database"), databaseSizes},
        {Group::SystemInfo, QStringLiteral("largest-tables"), QStringLiteral("Largest Tables"),
         QStringLiteral("Tables and materialized views by total size, in this database"),
         largestTables},
        {Group::SystemInfo, QStringLiteral("cache-hit"), QStringLiteral("Cache Hit Ratio"),
         QStringLiteral("Share of block reads served from shared buffers"), cacheHitRatio},
        {Group::SystemInfo, QStringLiteral("index-usage"), QStringLiteral("Index Usage"),
         QStringLiteral("Least used indexes first, in this database"), indexUsage},
        {Group::SystemInfo, QStringLiteral("vacuum"), QStringLiteral("Vacuum Status"),
         QStringLiteral("Dead rows and last (auto)vacuum and analyze, in this database"),
         vacuumStatus},
        {Group::SystemInfo, QStringLiteral("vacuum-progress"), QStringLiteral("Vacuum Progress"),
         QStringLiteral("Vacuums running now"), vacuumProgress},
        {Group::SystemInfo, QStringLiteral("replication"), QStringLiteral("Replication"),
         QStringLiteral("Connected standbys and their lag"), replication},
        {Group::SystemInfo, QStringLiteral("replication-slots"),
         QStringLiteral("Replication Slots"), QStringLiteral("Slots and the WAL they retain"),
         replicationSlots},
        {Group::SystemInfo, QStringLiteral("checkpoints"), QStringLiteral("Checkpoints"),
         QStringLiteral("Checkpointer statistics"), checkpoints},
        {Group::SystemInfo, QStringLiteral("wal"), QStringLiteral("WAL Statistics"),
         QStringLiteral("WAL generation since the last statistics reset"), wal},
        {Group::SystemInfo, QStringLiteral("io"), QStringLiteral("I/O Statistics"),
         QStringLiteral("Reads and writes by backend type and context"), io},
        {Group::SystemInfo, QStringLiteral("top-statements"), QStringLiteral("Top Statements"),
         QStringLiteral("Most expensive statements; needs the pg_stat_statements extension"),
         topStatements},
    };
    return queries;
}

} // namespace slonisko::catalog
