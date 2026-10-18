// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Analysis.h"

#include "quality/Checks.h"
#include "docgen/Pages.h"

#include <QLocale>

#include <algorithm>
#include <cmath>
#include <limits>

namespace slonisko::quality {

using docgen::quoteIdentifier;
using docgen::quoteQualified;

namespace {

QString number(qint64 value)
{
    return QLocale(QLocale::English).toString(value);
}

QString percent(double share)
{
    return QStringLiteral("%1 %").arg(share * 100, 0, 'f', share >= 0.995 || share < 0.1 ? 1 : 0);
}

QString duration(double seconds)
{
    const qint64 s = qint64(seconds);
    if (s >= 2 * 86400)
        return QStringLiteral("%1 days").arg(s / 86400);
    if (s >= 3600)
        return QStringLiteral("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
    if (s >= 60)
        return QStringLiteral("%1 min").arg(s / 60);
    return QStringLiteral("%1 s").arg(s);
}

QString literal(const QString &text)
{
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QLatin1String("''"))
        + QLatin1Char('\'');
}

struct Grade
{
    Section section;
    Severity severity;
};

Grade grade(Check check)
{
    switch (check) {
    case Check::NoPrimaryKey:
    case Check::ForeignKeyTypeMismatch:
    case Check::RowSecurity:
    case Check::SecurityDefiner:
    case Check::SequenceTypeMismatch:
        return {Section::Schema, Severity::Warning};
    case Check::NeedsQuoting:
    case Check::DiscouragedType:
    case Check::Undocumented:
        return {Section::Schema, Severity::Notice};
    case Check::UnindexedForeignKey:
    case Check::InvalidIndex:
    case Check::DuplicateIndex:
    case Check::RedundantIndex:
        return {Section::Indexes, Severity::Warning};
    case Check::NotValidConstraint:
    case Check::DisabledTrigger:
        return {Section::Dependencies, Severity::Warning};
    case Check::UnusedSequence:
        return {Section::Dependencies, Severity::Notice};
    case Check::DuplicateConstraint:
    case Check::DuplicateTrigger:
        return {Section::Duplicates, Severity::Warning};
    case Check::IdenticalDefinition:
    case Check::NameCaseClash:
        return {Section::Duplicates, Severity::Notice};
    }
    return {Section::Schema, Severity::Notice};
}

QString checkId(Check check)
{
    // The title, as an anchor: "duplicate-indexes".
    QString id;
    for (const QChar c : checkTitle(check).toLower()) {
        if (c.isLetterOrNumber())
            id += c;
        else if (!id.endsWith(QLatin1Char('-')))
            id += QLatin1Char('-');
    }
    while (id.endsWith(QLatin1Char('-')))
        id.chop(1);
    return id;
}

QString findingObject(const Finding &finding)
{
    QString name = finding.schema + QLatin1Char('.');
    if (!finding.relation.isEmpty())
        name += finding.relation;
    if (!finding.object.isEmpty())
        name += (finding.relation.isEmpty() ? QString() : QStringLiteral(" ")) + finding.object;
    return name;
}

void addChecks(const docgen::Model &model, std::vector<Topic> &topics)
{
    const std::vector<Finding> findings = runChecks(model);
    for (const Check check : allChecks()) {
        const Grade g = grade(check);
        Topic topic {g.section, checkId(check), checkTitle(check), checkExplanation(check), {}, {}};
        // COMMENT ON with an empty comment is a template, not a fix.
        topic.scripted = check != Check::Undocumented;
        for (const Finding &finding : findings) {
            if (finding.check == check)
                topic.issues.push_back(
                    {g.severity, findingObject(finding), finding.message, finding.fix});
        }
        topics.push_back(std::move(topic));
    }
}

class HealthAnalysis
{
public:
    HealthAnalysis(const docgen::Model &model, const Health &health, const Thresholds &thresholds)
        : m_model(model), m_health(health), m_t(thresholds)
    { }

    void run(std::vector<Topic> &topics);

private:
    Topic topic(Section section, const char *id, const QString &title, const QString &explanation,
                std::initializer_list<Part> needs) const;
    QString tableName(const QString &schema, const QString &name) const
    {
        return quoteQualified(schema, name);
    }

    Topic unusedIndexes() const;
    Topic sequentialScans() const;
    Topic routineErrors() const;
    Topic collationVersion() const;
    Topic extensionUpdates() const;
    Topic version() const;
    Topic visibility() const;
    Topic settings() const;
    Topic connections() const;
    Topic transactions() const;
    Topic prepared() const;
    Topic slots() const;
    Topic wraparound() const;
    Topic vacuum() const;
    Topic autovacuumOff() const;
    Topic statistics() const;
    Topic sequences() const;
    Topic cache() const;
    Topic databaseStatistics() const;
    Topic publicCreate() const;

    const docgen::Model &m_model;
    const Health &m_health;
    const Thresholds &m_t;
};

void HealthAnalysis::run(std::vector<Topic> &topics)
{
    using Make = Topic (HealthAnalysis::*)() const;
    for (const Make make : {&HealthAnalysis::unusedIndexes,      &HealthAnalysis::sequentialScans,
                            &HealthAnalysis::routineErrors,      &HealthAnalysis::collationVersion,
                            &HealthAnalysis::extensionUpdates,   &HealthAnalysis::version,
                            &HealthAnalysis::visibility,         &HealthAnalysis::settings,
                            &HealthAnalysis::connections,        &HealthAnalysis::transactions,
                            &HealthAnalysis::prepared,           &HealthAnalysis::slots,
                            &HealthAnalysis::wraparound,         &HealthAnalysis::vacuum,
                            &HealthAnalysis::autovacuumOff,      &HealthAnalysis::statistics,
                            &HealthAnalysis::sequences,          &HealthAnalysis::cache,
                            &HealthAnalysis::databaseStatistics, &HealthAnalysis::publicCreate})
        topics.push_back((this->*make)());
}

Topic HealthAnalysis::topic(Section section, const char *id, const QString &title,
                            const QString &explanation, std::initializer_list<Part> needs) const
{
    Topic t {section, QString::fromLatin1(id), title, explanation, {}, {}};
    for (const Part part : needs) {
        for (const Unavailable &u : m_health.unavailable) {
            if (u.part == part)
                t.unavailable = QStringLiteral("Could not read the %1: %2")
                                    .arg(partTitle(part).toLower(), u.error);
        }
    }
    return t;
}

Topic HealthAnalysis::unusedIndexes() const
{
    Topic t = topic(Section::Indexes, "unused-indexes", QStringLiteral("Unused indexes"),
                    QStringLiteral("Indexes no query has used since the statistics were last "
                                   "reset, which still cost space and every write. Unique ones "
                                   "and those behind a constraint are left out, as they enforce "
                                   "something. A standby keeps statistics of its own: an index "
                                   "only read there looks unused here."),
                    {Part::Indexes, Part::Database});
    if (!t.unavailable.isEmpty())
        return t;
    const qint64 age = m_health.database.statsAgeSeconds;
    if (age < m_t.statisticsAgeSeconds) {
        t.unavailable = QStringLiteral("The statistics are only %1 old: too soon to tell an "
                                       "unused index from one used now and then.")
                            .arg(duration(double(age)));
        return t;
    }
    for (const IndexHealth &index : m_health.indexes) {
        if (index.scans > 0 || index.enforcing || index.size < m_t.unusedIndexBytes)
            continue;
        t.issues.push_back(
            {Severity::Warning,
             index.schema + QLatin1Char('.') + index.table + QLatin1Char(' ') + index.name,
             QStringLiteral("Never used in %1; %2.")
                 .arg(duration(double(age)), docgen::formatSize(index.size)),
             QStringLiteral("DROP INDEX CONCURRENTLY %1;")
                 .arg(quoteQualified(index.schema, index.name))});
    }
    return t;
}

Topic HealthAnalysis::sequentialScans() const
{
    Topic t = topic(Section::Indexes, "sequential-scans",
                    QStringLiteral("Tables read whole more than through an index"),
                    QStringLiteral("Big tables that queries scan from start to end more often "
                                   "than they look anything up in an index: often a sign of an "
                                   "index missing. Which one, the queries tell; "
                                   "pg_stat_statements shows the ones that take longest."),
                    {Part::Tables});
    for (const TableHealth &table : m_health.tables) {
        if (table.liveRows < m_t.sequentialScanRows || table.seqScans <= table.indexScans)
            continue;
        t.issues.push_back(
            {Severity::Warning,
             table.schema + QLatin1Char('.') + table.name,
             QStringLiteral("%1 sequential scans reading %2 rows each on average, %3 index "
                            "scans; %4 rows.")
                 .arg(number(table.seqScans), number(table.seqRowsRead / table.seqScans),
                      number(table.indexScans), number(table.liveRows)),
             {}});
    }
    return t;
}

Topic HealthAnalysis::routineErrors() const
{
    Topic t
        = topic(Section::Dependencies, "routine-errors", QStringLiteral("Routines that would fail"),
                QStringLiteral("PL/pgSQL is only compiled when a routine is first called, "
                               "so one referring to a table, column or function that is "
                               "gone fails only then. plpgsql_check finds those errors "
                               "without running anything."),
                {Part::Routines});
    if (!t.unavailable.isEmpty())
        return t;
    if (!m_health.routinesChecked) {
        t.unavailable = QStringLiteral("Needs the plpgsql_check extension in the database.");
        return t;
    }
    for (const RoutineError &error : m_health.routineErrors)
        t.issues.push_back({Severity::Warning,
                            error.schema + QLatin1Char('.') + error.routine,
                            error.line > 0
                                ? QStringLiteral("Line %1: %2").arg(error.line).arg(error.message)
                                : error.message,
                            {}});
    return t;
}

Topic HealthAnalysis::collationVersion() const
{
    Topic t = topic(Section::Dependencies, "collation-version",
                    QStringLiteral("Collation version changed"),
                    QStringLiteral("Indexes on text are sorted the way the operating system's "
                                   "collation sorted when they were built. When an upgrade of "
                                   "the C library or ICU changes it, they may no longer find "
                                   "what they hold, and unique ones let duplicates in."),
                    {Part::Database});
    const DatabaseHealth &d = m_health.database;
    if (!d.collationVersion.isEmpty() && !d.actualCollationVersion.isEmpty()
        && d.collationVersion != d.actualCollationVersion)
        t.issues.push_back(
            {Severity::Critical, d.name,
             QStringLiteral("Recorded as %1, the system now has %2.")
                 .arg(d.collationVersion, d.actualCollationVersion),
             QStringLiteral("REINDEX DATABASE %1;\nALTER DATABASE %1 REFRESH COLLATION VERSION;")
                 .arg(quoteIdentifier(d.name))});
    return t;
}

Topic HealthAnalysis::extensionUpdates() const
{
    Topic t = topic(Section::Dependencies, "extension-updates",
                    QStringLiteral("Extensions behind their installed version"),
                    QStringLiteral("The server has a newer version of the extension than the "
                                   "database uses, typically after a package upgrade. The "
                                   "fixes in it apply only once the extension is updated."),
                    {Part::Extensions});
    for (const ExtensionUpdate &e : m_health.extensionUpdates)
        t.issues.push_back(
            {Severity::Notice, e.name,
             QStringLiteral("Version %1 in the database, %2 available.")
                 .arg(e.installed, e.available),
             QStringLiteral("ALTER EXTENSION %1 UPDATE;").arg(quoteIdentifier(e.name))});
    return t;
}

Topic HealthAnalysis::version() const
{
    Topic t = topic(Section::Health, "version", QStringLiteral("Server version"),
                    QStringLiteral("Each major version gets fixes for five years. After that, "
                                   "security holes and bugs found in it stay."),
                    {Part::Server});
    const QDate end = endOfLife(m_health.versionNumber);
    if (!end.isValid() || !m_health.today.isValid())
        return t;
    const qint64 days = m_health.today.daysTo(end);
    if (days < 0)
        t.issues.push_back(
            {Severity::Critical,
             m_health.version,
             QStringLiteral("No longer supported since %1.").arg(end.toString(Qt::ISODate)),
             {}});
    else if (days <= m_t.endOfLifeWarningDays)
        t.issues.push_back(
            {Severity::Warning,
             m_health.version,
             QStringLiteral("Supported until %1 only.").arg(end.toString(Qt::ISODate)),
             {}});
    return t;
}

Topic HealthAnalysis::visibility() const
{
    Topic t = topic(Section::Health, "visibility", QStringLiteral("What the report could see"),
                    QStringLiteral("Other users' sessions and some statistics are only shown to "
                                   "superusers and members of pg_monitor."),
                    {Part::Server});
    if (m_health.has(Part::Server) && !m_health.monitor)
        t.issues.push_back(
            {Severity::Notice, m_health.user,
             QStringLiteral("Neither a superuser nor a member of pg_monitor: "
                            "what other users do is missing here."),
             QStringLiteral("GRANT pg_monitor TO %1;").arg(quoteIdentifier(m_health.user))});
    for (const Unavailable &u : m_health.unavailable)
        t.issues.push_back({Severity::Notice,
                            partTitle(u.part),
                            QStringLiteral("Could not be read: %1").arg(u.error),
                            {}});
    return t;
}

Topic HealthAnalysis::settings() const
{
    Topic t = topic(Section::Health, "settings", QStringLiteral("Settings"),
                    QStringLiteral("Settings that put data at risk, keep autovacuum from its "
                                   "work, or are still at a default meant for a small machine; "
                                   "and changes that wait for a restart."),
                    {Part::Settings});
    const auto add = [&](const char *name, auto bad, Severity severity, const QString &why) {
        const Setting *s = m_health.setting(QString::fromLatin1(name));
        if (s && bad(s->value))
            t.issues.push_back(
                {severity, QStringLiteral("%1 = %2").arg(s->name, s->display()), why, {}});
    };
    const auto off = [](const QString &v) { return v == QLatin1String("off"); };
    add("fsync", off, Severity::Critical,
        QStringLiteral("Writes are not flushed to disk: a crash of the machine can corrupt the "
                       "whole cluster."));
    add("full_page_writes", off, Severity::Critical,
        QStringLiteral("A crash during a write can leave torn pages that recovery cannot "
                       "repair."));
    add("autovacuum", off, Severity::Critical,
        QStringLiteral("Nothing vacuums or analyzes on its own: tables bloat, statistics go "
                       "stale, and in the end the server stops to prevent transaction ID "
                       "wraparound."));
    add("track_counts", off, Severity::Critical,
        QStringLiteral("Autovacuum relies on these statistics to know what to vacuum."));
    add("synchronous_commit", off, Severity::Notice,
        QStringLiteral("A crash can lose the last transactions reported as committed."));
    add("data_checksums", off, Severity::Notice,
        QStringLiteral("Pages are not checksummed, so corruption on disk goes unnoticed until "
                       "it causes errors."));
    // 16384 pages of 8 kB, the built-in default.
    add(
        "shared_buffers", [](const QString &v) { return v == QLatin1String("16384"); },
        Severity::Notice,
        QStringLiteral("Still the built-in 128MB; a quarter of the memory is a common start "
                       "on a machine that runs only PostgreSQL."));
    add(
        "max_connections", [](const QString &v) { return v.toInt() > 500; }, Severity::Notice,
        QStringLiteral("Each connection is a process; a connection pooler serves many clients "
                       "with far fewer."));
    for (const Setting &s : m_health.settings) {
        if (s.pendingRestart)
            t.issues.push_back(
                {Severity::Warning,
                 s.name,
                 QStringLiteral("Changed in the configuration, but takes effect only when the "
                                "server is restarted; %1 still applies.")
                     .arg(s.display()),
                 {}});
    }
    return t;
}

Topic HealthAnalysis::connections() const
{
    Topic t = topic(Section::Health, "connections", QStringLiteral("Connections"),
                    QStringLiteral("Once max_connections is reached, new clients are turned "
                                   "away, the applications included."),
                    {Part::Server});
    const int available = m_health.maxConnections - m_health.reservedConnections;
    if (available <= 0)
        return t;
    const double share = double(m_health.connections) / available;
    if (share >= m_t.connectionsShare)
        t.issues.push_back(
            {share >= m_t.connectionsCriticalShare ? Severity::Critical : Severity::Warning,
             QStringLiteral("max_connections"),
             QStringLiteral("%1 of %2 connections in use (%3).")
                 .arg(m_health.connections)
                 .arg(available)
                 .arg(percent(share)),
             {}});
    return t;
}

Topic HealthAnalysis::transactions() const
{
    Topic t = topic(Section::Health, "transactions", QStringLiteral("Long transactions"),
                    QStringLiteral("An open transaction holds its locks, and keeps vacuum from "
                                   "removing any row that changed since it began, in every "
                                   "table of the database. One idle in a transaction is "
                                   "usually an application that forgot to commit; "
                                   "idle_in_transaction_session_timeout ends those."),
                    {Part::Sessions});
    for (const Session &s : m_health.sessions) {
        const bool idle = s.state.startsWith(QLatin1String("idle in transaction"));
        if (idle ? s.stateSeconds < m_t.idleInTransactionSeconds
                 : s.transactionSeconds < m_t.longTransactionSeconds)
            continue;
        QString object = QStringLiteral("pid %1, %2").arg(s.pid).arg(s.user);
        if (!s.application.isEmpty())
            object += QStringLiteral(" (%1)").arg(s.application);
        const QString message = idle
            ? QStringLiteral("Idle in a transaction for %1, open for %2. Last statement: %3")
                  .arg(duration(s.stateSeconds), duration(s.transactionSeconds), s.query)
            : QStringLiteral("In a transaction for %1, now %2: %3")
                  .arg(duration(s.transactionSeconds), s.state, s.query);
        t.issues.push_back(
            {s.transactionSeconds >= m_t.criticalTransactionSeconds ? Severity::Critical
                                                                    : Severity::Warning,
             object, message, QStringLiteral("SELECT pg_terminate_backend(%1);").arg(s.pid)});
    }
    return t;
}

Topic HealthAnalysis::prepared() const
{
    Topic t = topic(Section::Health, "prepared-transactions",
                    QStringLiteral("Forgotten prepared transactions"),
                    QStringLiteral("A transaction prepared for a two-phase commit lives on, "
                                   "restarts included, until it is committed or rolled back: "
                                   "it holds its locks and keeps vacuum from cleaning up, and "
                                   "can lead to wraparound."),
                    {Part::Prepared});
    for (const PreparedTransaction &p : m_health.prepared) {
        if (p.ageSeconds < m_t.preparedSeconds)
            continue;
        t.issues.push_back({Severity::Critical, p.gid,
                            QStringLiteral("Prepared %1 ago by %2 in %3.")
                                .arg(duration(p.ageSeconds), p.owner, p.database),
                            QStringLiteral("ROLLBACK PREPARED %1;").arg(literal(p.gid))});
    }
    return t;
}

Topic HealthAnalysis::slots() const
{
    Topic t = topic(Section::Health, "replication-slots", QStringLiteral("Replication slots"),
                    QStringLiteral("A slot keeps the WAL its consumer has not received yet. "
                                   "One nobody consumes keeps it forever, until the disk is "
                                   "full; a logical one also holds vacuum back."),
                    {Part::Slots});
    for (const ReplicationSlot &slot : m_health.slots) {
        QStringList problems;
        Severity severity = Severity::Warning;
        if (slot.walStatus == QLatin1String("lost")) {
            problems << QStringLiteral("Lost: the WAL it needs is gone, so it is of no use.");
            severity = Severity::Critical;
        } else if (!slot.active) {
            problems << QStringLiteral("Not in use.");
        }
        if (slot.retainedWal >= m_t.retainedWalBytes) {
            problems
                << QStringLiteral("Keeps %1 of WAL.").arg(docgen::formatSize(slot.retainedWal));
            severity = Severity::Critical;
        } else if (!problems.isEmpty() && slot.retainedWal >= 0) {
            problems
                << QStringLiteral("Keeps %1 of WAL.").arg(docgen::formatSize(slot.retainedWal));
        }
        if (problems.isEmpty())
            continue;
        t.issues.push_back(
            {severity, QStringLiteral("%1 (%2)").arg(slot.name, slot.type),
             problems.join(QLatin1Char(' ')),
             QStringLiteral("SELECT pg_drop_replication_slot(%1);").arg(literal(slot.name))});
    }
    return t;
}

Topic HealthAnalysis::wraparound() const
{
    Topic t = topic(Section::Health, "wraparound", QStringLiteral("Transaction ID wraparound"),
                    QStringLiteral("Transaction IDs are 32 bits. Vacuum freezes old rows so the "
                                   "IDs can be used again; when it falls two billion behind, "
                                   "the server stops taking writes. Usually autovacuum keeps up "
                                   "on its own; when an age keeps growing, something keeps it "
                                   "from that: a long transaction, a prepared one, a slot."),
                    {Part::Databases, Part::Tables});
    const auto severity = [this](qint64 age) {
        return age >= m_t.wraparoundCriticalAge ? Severity::Critical : Severity::Warning;
    };
    for (const ServerDatabase &d : m_health.databases) {
        const qint64 age = std::max(d.xidAge, d.multixactAge);
        if (age < m_t.wraparoundAge)
            continue;
        t.issues.push_back(
            {severity(age), d.name,
             QStringLiteral("Transaction ID age %1, multixact age %2.")
                 .arg(number(d.xidAge), number(d.multixactAge)),
             d.name == m_health.database.name ? QStringLiteral("VACUUM (FREEZE);") : QString()});
    }
    for (const TableHealth &table : m_health.tables) {
        if (table.xidAge < m_t.wraparoundAge)
            continue;
        t.issues.push_back(
            {severity(table.xidAge), table.schema + QLatin1Char('.') + table.name,
             QStringLiteral("Transaction ID age %1.").arg(number(table.xidAge)),
             QStringLiteral("VACUUM (FREEZE) %1;").arg(tableName(table.schema, table.name))});
    }
    return t;
}

Topic HealthAnalysis::vacuum() const
{
    Topic t = topic(Section::Health, "dead-rows", QStringLiteral("Dead rows"),
                    QStringLiteral("An updated or deleted row stays until vacuum removes it. "
                                   "Many of them make a table, and its scans, bigger than its "
                                   "data; autovacuum not keeping up is often a long "
                                   "transaction's doing, or settings too timid for the "
                                   "table."),
                    {Part::Tables});
    for (const TableHealth &table : m_health.tables) {
        const qint64 all = table.liveRows + table.deadRows;
        if (table.deadRows < m_t.deadRows || all == 0
            || double(table.deadRows) / double(all) < m_t.deadRowsShare)
            continue;
        t.issues.push_back(
            {Severity::Warning, table.schema + QLatin1Char('.') + table.name,
             QStringLiteral("%1 dead rows beside %2 live ones (%3); last vacuumed %4.")
                 .arg(number(table.deadRows), number(table.liveRows),
                      percent(double(table.deadRows) / double(all)),
                      table.lastVacuum.isEmpty() ? QStringLiteral("never") : table.lastVacuum),
             QStringLiteral("VACUUM (ANALYZE) %1;").arg(tableName(table.schema, table.name))});
    }
    return t;
}

Topic HealthAnalysis::autovacuumOff() const
{
    Topic t
        = topic(Section::Health, "autovacuum-off", QStringLiteral("Tables autovacuum leaves alone"),
                QStringLiteral("autovacuum_enabled = false keeps autovacuum from vacuuming "
                               "and analyzing the table, except to prevent wraparound: "
                               "unless a job of its own does that, it bloats and its "
                               "statistics go stale."),
                {});
    for (const docgen::Relation &relation : m_model.relations) {
        const QStringList options = relation.options.split(QLatin1String(", "));
        const bool off = std::ranges::any_of(options, [](const QString &option) {
            const QString o = option.toLower();
            return o.startsWith(QLatin1String("autovacuum_enabled="))
                && QStringList {QStringLiteral("false"), QStringLiteral("off"),
                                QStringLiteral("no"),    QStringLiteral("0"),
                                QStringLiteral("f"),     QStringLiteral("n")}
                       .contains(o.mid(o.indexOf(QLatin1Char('=')) + 1));
        });
        if (off)
            t.issues.push_back({Severity::Warning, relation.qualifiedName(),
                                QStringLiteral("autovacuum_enabled is off."),
                                QStringLiteral("ALTER TABLE %1 RESET (autovacuum_enabled);")
                                    .arg(tableName(relation.schema, relation.name))});
    }
    return t;
}

Topic HealthAnalysis::statistics() const
{
    Topic t = topic(Section::Health, "planner-statistics",
                    QStringLiteral("Planner statistics out of date"),
                    QStringLiteral("The planner chooses how to run a query by what ANALYZE "
                                   "found in the table. A table never analyzed, or changed "
                                   "much since, gets plans made for data it no longer has."),
                    {Part::Tables});
    for (const TableHealth &table : m_health.tables) {
        const QString name = table.schema + QLatin1Char('.') + table.name;
        const QString fix = QStringLiteral("ANALYZE %1;").arg(tableName(table.schema, table.name));
        if (!table.analyzed && (table.liveRows > 0 || table.modifiedSinceAnalyze > 0)) {
            t.issues.push_back(
                {Severity::Warning, name,
                 table.liveRows > 0
                     ? QStringLiteral("Never analyzed; %1 rows.").arg(number(table.liveRows))
                     : QStringLiteral("Never analyzed; %1 rows changed.")
                           .arg(number(table.modifiedSinceAnalyze)),
                 fix});
            continue;
        }
        const double limit = std::max(double(m_t.staleAnalyzeRows),
                                      m_t.staleAnalyzeShare * double(table.liveRows));
        if (table.analyzed && double(table.modifiedSinceAnalyze) >= limit)
            t.issues.push_back(
                {Severity::Notice, name,
                 QStringLiteral("%1 rows changed since it was analyzed on %2; %3 rows.")
                     .arg(number(table.modifiedSinceAnalyze), table.lastAnalyze,
                          number(table.liveRows)),
                 fix});
    }
    return t;
}

Topic HealthAnalysis::sequences() const
{
    Topic t = topic(Section::Health, "sequences", QStringLiteral("Sequences running out"),
                    QStringLiteral("A sequence at its end makes every insert that needs a "
                                   "number fail. A column narrower than its sequence ends "
                                   "sooner still."),
                    {Part::Sequences});
    const auto limit = [](const QString &type) -> qint64 {
        if (type == QLatin1String("smallint"))
            return std::numeric_limits<qint16>::max();
        if (type == QLatin1String("integer"))
            return std::numeric_limits<qint32>::max();
        return std::numeric_limits<qint64>::max();
    };
    for (const SequenceHealth &s : m_health.sequences) {
        if (s.cycle || !s.lastValue || s.increment == 0)
            continue;
        qint64 maximum = s.maximum;
        qint64 minimum = s.minimum;
        const docgen::Sequence *catalog = nullptr;
        for (const docgen::Sequence &candidate : m_model.sequences) {
            if (candidate.schema == s.schema && candidate.name == s.name)
                catalog = &candidate;
        }
        const docgen::Column *column = nullptr;
        const docgen::Relation *table = nullptr;
        if (catalog) {
            const QStringList owner = catalog->ownedBy.split(QLatin1Char('.'));
            table = owner.size() == 3 ? m_model.relation(owner[0], owner[1]) : nullptr;
            column = table ? table->column(owner[2]) : nullptr;
            if (column) {
                maximum = std::min(maximum, limit(column->type));
                minimum = std::max(minimum, -limit(column->type));
            }
        }
        const double range = double(maximum) - double(minimum);
        if (range <= 0)
            continue;
        const double used = s.increment > 0 ? (double(*s.lastValue) - double(minimum)) / range
                                            : (double(maximum) - double(*s.lastValue)) / range;
        if (used < m_t.sequenceUsedShare)
            continue;
        QString fix;
        if (column && limit(column->type) < limit(QStringLiteral("bigint")))
            fix = QStringLiteral("ALTER TABLE %1 ALTER COLUMN %2 TYPE bigint;\n")
                      .arg(tableName(table->schema, table->name), quoteIdentifier(column->name));
        if (catalog && catalog->type != QLatin1String("bigint"))
            fix += QStringLiteral("ALTER SEQUENCE %1 AS bigint;")
                       .arg(quoteQualified(s.schema, s.name));
        t.issues.push_back(
            {used >= m_t.sequenceCriticalShare ? Severity::Critical : Severity::Warning,
             s.schema + QLatin1Char('.') + s.name,
             QStringLiteral("At %1 of %2: %3 used%4.")
                 .arg(number(*s.lastValue), number(s.increment > 0 ? maximum : minimum),
                      percent(used),
                      column ? QStringLiteral(", for %1.%2 (%3)")
                                   .arg(table->name, column->name, column->type)
                             : QString()),
             fix.trimmed()});
    }
    return t;
}

Topic HealthAnalysis::cache() const
{
    Topic t = topic(Section::Health, "cache-hit", QStringLiteral("Cache hit ratio"),
                    QStringLiteral("How many of the blocks read came from shared_buffers rather "
                                   "than the operating system or the disk. A busy database "
                                   "that misses often may need more memory, or has queries "
                                   "reading far more than they need."),
                    {Part::Database});
    const DatabaseHealth &d = m_health.database;
    const qint64 all = d.blocksHit + d.blocksRead;
    if (all < m_t.cacheBlocks)
        return t;
    const double share = double(d.blocksHit) / double(all);
    if (share < m_t.cacheHitShare)
        t.issues.push_back(
            {share < m_t.cacheHitCriticalShare ? Severity::Warning : Severity::Notice,
             d.name,
             QStringLiteral("%1 of %2 blocks read from the cache.")
                 .arg(percent(share), number(all)),
             {}});
    return t;
}

Topic HealthAnalysis::databaseStatistics() const
{
    Topic t = topic(Section::Health, "database-statistics", QStringLiteral("Errors and spills"),
                    QStringLiteral("What the database's statistics have counted since they "
                                   "were reset: checksum failures are damaged pages, deadlocks "
                                   "are transactions the server had to cancel, temporary files "
                                   "are sorts and hashes that did not fit work_mem."),
                    {Part::Database});
    const DatabaseHealth &d = m_health.database;
    if (d.checksumFailures > 0)
        t.issues.push_back({Severity::Critical,
                            d.name,
                            QStringLiteral("%1 checksum failures: pages on disk are damaged.")
                                .arg(number(d.checksumFailures)),
                            {}});
    if (d.deadlocks > 0)
        t.issues.push_back({Severity::Notice,
                            d.name,
                            QStringLiteral("%1 deadlocks.").arg(number(d.deadlocks)),
                            {}});
    if (d.tempBytes >= m_t.tempBytes)
        t.issues.push_back({Severity::Notice,
                            d.name,
                            QStringLiteral("%1 temporary files, %2 in all.")
                                .arg(number(d.tempFiles), docgen::formatSize(d.tempBytes)),
                            {}});
    const qint64 transactions = d.commits + d.rollbacks;
    if (transactions >= 1000 && double(d.rollbacks) / double(transactions) > 0.1)
        t.issues.push_back({Severity::Notice,
                            d.name,
                            QStringLiteral("%1 of the transactions were rolled back.")
                                .arg(percent(double(d.rollbacks) / double(transactions))),
                            {}});
    return t;
}

Topic HealthAnalysis::publicCreate() const
{
    Topic t = topic(Section::Health, "public-create",
                    QStringLiteral("Schemas anyone may create objects in"),
                    QStringLiteral("When every role may create objects in a schema, one can "
                                   "put a function or operator there that others' queries pick "
                                   "up instead of the one meant. Since PostgreSQL 15, public "
                                   "no longer allows it by default."),
                    {Part::Privileges});
    for (const QString &schema : m_health.publicCreateSchemas)
        t.issues.push_back({Severity::Warning, schema,
                            QStringLiteral("PUBLIC has the CREATE privilege."),
                            QStringLiteral("REVOKE CREATE ON SCHEMA %1 FROM PUBLIC;")
                                .arg(quoteIdentifier(schema))});
    return t;
}

} // namespace

QString severityName(Severity severity)
{
    switch (severity) {
    case Severity::Critical:
        return QStringLiteral("Critical");
    case Severity::Warning:
        return QStringLiteral("Warning");
    case Severity::Notice:
        return QStringLiteral("Notice");
    }
    return {};
}

std::vector<Section> allSections()
{
    return {Section::Schema, Section::Indexes, Section::Dependencies, Section::Duplicates,
            Section::Health};
}

QString sectionTitle(Section section)
{
    switch (section) {
    case Section::Schema:
        return QStringLiteral("Schema advice");
    case Section::Indexes:
        return QStringLiteral("Indexes");
    case Section::Dependencies:
        return QStringLiteral("Broken dependencies");
    case Section::Duplicates:
        return QStringLiteral("Duplicates");
    case Section::Health:
        return QStringLiteral("Server and database health");
    }
    return {};
}

QString sectionExplanation(Section section)
{
    switch (section) {
    case Section::Schema:
        return QStringLiteral("How the tables, keys, types and routines are made: what makes "
                              "them harder to use, slower or less safe than they could be.");
    case Section::Indexes:
        return QStringLiteral("Indexes that are missing, the same twice, made redundant by "
                              "another, never used, or broken.");
    case Section::Dependencies:
        return QStringLiteral("What one object relies on and does not get: constraints and "
                              "triggers switched off or never checked, routines referring to "
                              "what is gone, indexes built for another collation.");
    case Section::Duplicates:
        return QStringLiteral("Constraints, triggers, views and routines that exist twice, and "
                              "names only their case tells apart.");
    case Section::Health:
        return QStringLiteral("How the server and the database are doing right now: version, "
                              "settings, connections, transactions, vacuum and what the "
                              "statistics have counted.");
    }
    return {};
}

QString sectionKey(Section section)
{
    switch (section) {
    case Section::Schema:
        return QStringLiteral("schema");
    case Section::Indexes:
        return QStringLiteral("indexes");
    case Section::Dependencies:
        return QStringLiteral("dependencies");
    case Section::Duplicates:
        return QStringLiteral("duplicates");
    case Section::Health:
        return QStringLiteral("health");
    }
    return {};
}

std::vector<Topic> analyse(const docgen::Model &model, const Health *health,
                           const Thresholds &thresholds)
{
    std::vector<Topic> topics;
    addChecks(model, topics);
    if (health)
        HealthAnalysis(model, *health, thresholds).run(topics);
    // Foreign keys without an index close the Indexes page: what is wrong
    // with the indexes there are comes before the ones to add.
    const QString unindexed = checkId(Check::UnindexedForeignKey);
    std::ranges::stable_sort(
        topics, {}, [&](const Topic &t) { return std::pair {int(t.section), t.id == unindexed}; });
    // The worst first within a topic.
    for (Topic &topic : topics)
        std::ranges::stable_sort(topic.issues, {}, [](const Issue &i) { return int(i.severity); });
    return topics;
}

QDate endOfLife(int serverVersion)
{
    // Five years after the first release, in the November round of minor
    // releases; www.postgresql.org/support/versioning.
    switch (serverVersion / 10000) {
    case 10:
        return {2022, 11, 10};
    case 11:
        return {2023, 11, 9};
    case 12:
        return {2024, 11, 21};
    case 13:
        return {2025, 11, 13};
    case 14:
        return {2026, 11, 12};
    case 15:
        return {2027, 11, 11};
    case 16:
        return {2028, 11, 9};
    case 17:
        return {2029, 11, 8};
    case 18:
        return {2030, 11, 14};
    default:
        return {};
    }
}

int count(const std::vector<Topic> &topics, Section section, Severity severity)
{
    int n = 0;
    for (const Topic &topic : topics) {
        if (topic.section == section)
            n += int(std::ranges::count(topic.issues, severity, &Issue::severity));
    }
    return n;
}

int count(const std::vector<Topic> &topics, Severity severity)
{
    int n = 0;
    for (const Section section : allSections())
        n += count(topics, section, severity);
    return n;
}

std::optional<Severity> worst(const std::vector<Topic> &topics)
{
    for (const Severity severity : {Severity::Critical, Severity::Warning, Severity::Notice}) {
        if (count(topics, severity) > 0)
            return severity;
    }
    return std::nullopt;
}

} // namespace slonisko::quality
