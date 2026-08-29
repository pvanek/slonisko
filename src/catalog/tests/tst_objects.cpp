// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Monitoring.h"
#include "catalog/Objects.h"
#include "pg/QueryRunner.h"

#include <QFile>
#include <QTest>

using namespace slonisko::catalog;
using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;
using slonisko::pg::QueryRunner;

namespace {

QByteArray serverConninfo()
{
    const QByteArray conninfo = qgetenv("SLONISKO_TEST_CONNINFO");
    if (!conninfo.isEmpty())
        return conninfo;
    QFile file(qEnvironmentVariable("SLONISKO_TEST_CONNINFO_FILE"));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll().trimmed();
}

const QByteArray Schema = "slonisko_objects_test";

const char *const Setup = R"sql(
DROP SCHEMA IF EXISTS slonisko_objects_test CASCADE;
DROP PUBLICATION IF EXISTS slonisko_objects_pub;
DROP EVENT TRIGGER IF EXISTS slonisko_objects_evt;
CREATE SCHEMA slonisko_objects_test;
SET search_path = slonisko_objects_test;
CREATE TABLE t (id int PRIMARY KEY, name text NOT NULL, dropped int, CHECK (id > 0));
ALTER TABLE t DROP COLUMN dropped;
CREATE INDEX t_name ON t (name);
CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER t_trg BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trg();
CREATE POLICY t_read ON t FOR SELECT USING (true);
CREATE TABLE parted (id int, d date) PARTITION BY RANGE (d);
CREATE TABLE parted_2026 PARTITION OF parted FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
CREATE VIEW v AS SELECT id FROM t;
CREATE MATERIALIZED VIEW mv AS SELECT id FROM t;
CREATE INDEX mv_id ON mv (id);
CREATE SEQUENCE s;
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE sql RETURN a + b;
CREATE PROCEDURE p(x int) LANGUAGE sql BEGIN ATOMIC SELECT x; END;
CREATE AGGREGATE mysum(int) (sfunc = int4pl, stype = int);
CREATE TYPE mood AS ENUM ('sad', 'happy');
CREATE TYPE pair AS (a int, b int);
CREATE TYPE span AS RANGE (subtype = int8);
CREATE DOMAIN posint AS int CHECK (VALUE > 0);
SET client_min_messages = error;
CREATE PUBLICATION slonisko_objects_pub FOR TABLE t;
CREATE FUNCTION evt() RETURNS event_trigger LANGUAGE plpgsql AS $$BEGIN END$$;
CREATE EVENT TRIGGER slonisko_objects_evt ON ddl_command_end EXECUTE FUNCTION evt();
)sql";

const char *const Teardown = R"sql(
DROP EVENT TRIGGER IF EXISTS slonisko_objects_evt;
DROP PUBLICATION IF EXISTS slonisko_objects_pub;
DROP SCHEMA IF EXISTS slonisko_objects_test CASCADE;
)sql";

} // namespace

class TestObjects : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        const QByteArray conninfo = serverConninfo();
        if (conninfo.isEmpty())
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        m_connection.open(conninfo);
        QTRY_COMPARE(m_connection.state(), Connection::State::Ready);
        const QueryOutcome setup = run(Setup);
        QVERIFY2(setup.ok(), qPrintable(setup.error));
        m_schema = oidOf("SELECT oid FROM pg_namespace WHERE nspname = '" + Schema + "'");
    }

    void cleanupTestCase()
    {
        if (m_connection.state() == Connection::State::Ready)
            run(Teardown);
    }

    void databaseFolders()
    {
        QVERIFY(names(Folder::Databases).contains(QStringLiteral("postgres")));
        QVERIFY(names(Folder::Schemas).contains(QString::fromUtf8(Schema)));
        QVERIFY(names(Folder::Extensions).contains(QStringLiteral("plpgsql")));
        QVERIFY(names(Folder::Publications).contains(QStringLiteral("slonisko_objects_pub")));
        QVERIFY(names(Folder::EventTriggers).contains(QStringLiteral("slonisko_objects_evt")));
        QVERIFY(names(Folder::Roles).contains(QStringLiteral("postgres")));
        QVERIFY(names(Folder::Tablespaces).contains(QStringLiteral("pg_default")));
    }

    void systemObjectsLast()
    {
        const auto schemas = list(Folder::Schemas);
        QVERIFY(!schemas.empty());
        const auto catalog
            = std::ranges::find(schemas, QStringLiteral("pg_catalog"), &DbObject::name);
        QVERIFY(catalog != schemas.end());
        QVERIFY(catalog->system);
        QVERIFY(!schemas.front().system);
        QVERIFY(std::ranges::none_of(schemas, [](const DbObject &o) {
            return o.name.startsWith(QLatin1String("pg_toast"));
        }));
    }

    void schemaFolders()
    {
        const auto tables = list(Folder::Tables, m_schema);
        QCOMPARE(namesOf(tables), (QStringList {QStringLiteral("parted"), QStringLiteral("t")}));
        QCOMPARE(tables[0].kind, ObjectKind::PartitionedTable); // Partitions are not listed.
        QCOMPARE(tables[1].kind, ObjectKind::Table);

        QCOMPARE(names(Folder::Views, m_schema), QStringList {QStringLiteral("v")});
        QCOMPARE(names(Folder::MaterializedViews, m_schema), QStringList {QStringLiteral("mv")});
        QCOMPARE(names(Folder::ForeignTables, m_schema), QStringList {});
        QCOMPARE(names(Folder::Sequences, m_schema), QStringList {QStringLiteral("s")});
        QCOMPARE(names(Folder::Functions, m_schema),
                 (QStringList {QStringLiteral("add(a integer, b integer)"), QStringLiteral("evt()"),
                               QStringLiteral("trg()")}));
        QCOMPARE(list(Folder::Functions, m_schema)[0].detail, QStringLiteral("integer"));
        QCOMPARE(names(Folder::Procedures, m_schema),
                 QStringList {QStringLiteral("p(IN x integer)")});
        QCOMPARE(names(Folder::Aggregates, m_schema),
                 QStringList {QStringLiteral("mysum(integer)")});
        // No array types, no row types of tables and views.
        QCOMPARE(
            names(Folder::Types, m_schema),
            (QStringList {QStringLiteral("mood"), QStringLiteral("pair"), QStringLiteral("span")}));
        QCOMPARE(names(Folder::Domains, m_schema), QStringList {QStringLiteral("posint")});
        QCOMPARE(list(Folder::Domains, m_schema)[0].detail, QStringLiteral("integer"));
    }

    void tableFolders()
    {
        const Oid t = oidOf("SELECT '" + Schema + ".t'::regclass::oid");
        const auto columns = list(Folder::Columns, t);
        QCOMPARE(namesOf(columns), (QStringList {QStringLiteral("id"), QStringLiteral("name")}));
        QCOMPARE(columns[0].oid, 1u); // Attribute numbers; the dropped column is gone.
        QCOMPARE(columns[1].detail, QStringLiteral("text not null"));

        const auto constraints = list(Folder::Constraints, t);
        QCOMPARE(namesOf(constraints),
                 (QStringList {QStringLiteral("t_id_check"), QStringLiteral("t_pkey")}));
        QCOMPARE(constraints[1].detail, QStringLiteral("primary key"));

        QCOMPARE(names(Folder::Indexes, t),
                 (QStringList {QStringLiteral("t_name"), QStringLiteral("t_pkey")}));
        QCOMPARE(names(Folder::Triggers, t), QStringList {QStringLiteral("t_trg")});
        QCOMPARE(names(Folder::Policies, t), QStringList {QStringLiteral("t_read")});
        QCOMPARE(list(Folder::Policies, t)[0].detail, QStringLiteral("select"));

        const Oid parted = oidOf("SELECT '" + Schema + ".parted'::regclass::oid");
        const auto partitions = list(Folder::Partitions, parted);
        QCOMPARE(namesOf(partitions), QStringList {QStringLiteral("parted_2026")});
        QCOMPARE(partitions[0].kind, ObjectKind::Table);
        QVERIFY(partitions[0].detail.startsWith(QLatin1String("FOR VALUES FROM")));

        const Oid mv = oidOf("SELECT '" + Schema + ".mv'::regclass::oid");
        QCOMPARE(names(Folder::Indexes, mv), QStringList {QStringLiteral("mv_id")});
    }

    void foldersOfEveryKindHaveQueries()
    {
        for (int k = 0; k <= int(ObjectKind::Tablespace); ++k) {
            for (Folder f : foldersOf(ObjectKind(k))) {
                QVERIFY(!folderTitle(f).isEmpty());
                const QueryOutcome o = run(folderQuery(f, m_schema));
                QVERIFY2(o.ok(), qPrintable(folderTitle(f) + QStringLiteral(": ") + o.error));
            }
        }
    }

    void monitoringQueriesRun()
    {
        for (const MonitoringQuery &q : monitoringQueries()) {
            const QueryOutcome o = run(q.sql(m_connection.serverVersion()));
            if (q.id == QLatin1String("top-statements") && o.sqlState == "42P01")
                continue; // pg_stat_statements is not installed.
            QVERIFY2(o.ok(), qPrintable(q.title + QStringLiteral(": ") + o.error));
            QVERIFY2(!o.results.empty() && o.results.back().columnCount() > 0, qPrintable(q.title));
        }
    }

    void monitoringQueriesForOlderServers()
    {
        // The branches for older servers must at least parse on this one.
        for (const MonitoringQuery &q : monitoringQueries()) {
            if (q.id != QLatin1String("checkpoints"))
                continue;
            const QueryOutcome o = run(q.sql(160000));
            // pg_stat_bgwriter lost these columns in 17.
            QVERIFY(m_connection.serverVersion() >= 170000 ? !o.ok() : o.ok());
        }
    }

private:
    QueryOutcome run(const QByteArray &sql)
    {
        std::optional<QueryOutcome> outcome;
        m_runner.run(sql, this, [&](const QueryOutcome &o) { outcome = o; });
        if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 10'000))
            return {{}, QStringLiteral("timed out"), {}};
        return *outcome;
    }

    Oid oidOf(const QByteArray &sql)
    {
        const QueryOutcome o = run(sql);
        return o.ok() && !o.results.empty() ? o.results[0].value(0, 0).toUInt() : 0;
    }

    std::vector<DbObject> list(Folder folder, Oid owner = 0)
    {
        const QueryOutcome o = run(folderQuery(folder, owner));
        if (!o.ok()) {
            qWarning("%s: %s", qPrintable(folderTitle(folder)), qPrintable(o.error));
            return {};
        }
        return parseFolder(folder, o.results.back());
    }

    static QStringList namesOf(const std::vector<DbObject> &objects)
    {
        QStringList out;
        for (const DbObject &o : objects)
            out << o.name;
        return out;
    }

    QStringList names(Folder folder, Oid owner = 0) { return namesOf(list(folder, owner)); }

    Connection m_connection;
    QueryRunner m_runner {&m_connection};
    Oid m_schema = 0;
};

QTEST_GUILESS_MAIN(TestObjects)
#include "tst_objects.moc"
