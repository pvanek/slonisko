// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Details.h"
#include "pg/QueryRunner.h"

#include <QFile>
#include <QTest>

using namespace slonisko::catalog;
using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;
using slonisko::pg::Result;

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

const char *const Setup = R"sql(
DROP SCHEMA IF EXISTS slonisko_details_test CASCADE;
CREATE SCHEMA slonisko_details_test;
SET search_path = slonisko_details_test;
CREATE TABLE parent (id int PRIMARY KEY);
CREATE TABLE t (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL,
    parent_id int REFERENCES parent (id),
    amount numeric(10, 2) DEFAULT 0,
    CHECK (amount >= 0));
CREATE INDEX t_name ON t (name);
COMMENT ON TABLE t IS 'Things';
COMMENT ON COLUMN t.name IS 'What it is called';
CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER t_trg BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trg();
CREATE VIEW v AS SELECT id, name FROM t WHERE amount > 0;
CREATE MATERIALIZED VIEW mv AS SELECT id FROM t;
CREATE INDEX mv_id ON mv (id);
CREATE SEQUENCE s START 5 INCREMENT 2;
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE sql RETURN a + b;
CREATE PROCEDURE p(x int) LANGUAGE sql BEGIN ATOMIC SELECT x; END;
CREATE AGGREGATE mysum(int) (sfunc = int4pl, stype = int);
CREATE TYPE mood AS ENUM ('sad', 'happy');
CREATE TYPE pair AS (a int, b int);
CREATE DOMAIN posint AS int NOT NULL CHECK (VALUE > 0);
ALTER TABLE t ENABLE ROW LEVEL SECURITY;
CREATE POLICY t_read ON t FOR SELECT USING (true);
SET client_min_messages = error;
DROP PUBLICATION IF EXISTS slonisko_details_pub;
CREATE PUBLICATION slonisko_details_pub FOR TABLE t;
DROP EVENT TRIGGER IF EXISTS slonisko_details_evt;
CREATE FUNCTION evt() RETURNS event_trigger LANGUAGE plpgsql AS $$BEGIN END$$;
CREATE EVENT TRIGGER slonisko_details_evt ON ddl_command_end EXECUTE FUNCTION evt();
)sql";

const char *const Teardown = R"sql(
DROP EVENT TRIGGER IF EXISTS slonisko_details_evt;
DROP PUBLICATION IF EXISTS slonisko_details_pub;
DROP SCHEMA IF EXISTS slonisko_details_test CASCADE;
)sql";

} // namespace

class TestDetails : public QObject
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
    }

    void cleanupTestCase()
    {
        if (m_connection.state() == Connection::State::Ready)
            run(Teardown);
    }

    void kindsWithDetails()
    {
        QVERIFY(hasDetails(ObjectKind::Table));
        QVERIFY(hasDetails(ObjectKind::View));
        QVERIFY(hasDetails(ObjectKind::MaterializedView));
        QVERIFY(hasDetails(ObjectKind::Extension));
        QVERIFY(hasDetails(ObjectKind::Sequence));
        QVERIFY(hasDetails(ObjectKind::Role));
        // A column is described by its table's Columns tab, not a page.
        QVERIFY(!hasDetails(ObjectKind::Column));
        QVERIFY(detailQueries(ObjectKind::Column, 1).empty());
    }

    void table()
    {
        const ObjectDetail detail = detailOf(ObjectKind::Table, oidOf("'slonisko_details_test.t'"));
        QCOMPARE(detail.title, QStringLiteral("slonisko_details_test.t"));
        QCOMPARE(detail.subtitle, kindName(ObjectKind::Table));
        QVERIFY(property(detail, QStringLiteral("owner")).size() > 0);
        QCOMPARE(property(detail, QStringLiteral("comment")), QStringLiteral("Things"));
        QVERIFY(property(detail, QStringLiteral("size")).contains(QLatin1String("bytes"))
                || property(detail, QStringLiteral("size")).contains(QLatin1String("kB")));

        const DetailTable columns = table(detail, QStringLiteral("Columns"));
        QCOMPARE(columns.rows.size(), 4u);
        QCOMPARE(columns.rows[0][1], QStringLiteral("id"));
        QCOMPARE(columns.rows[0][2], QStringLiteral("bigint"));
        QCOMPARE(columns.rows[0][3], QStringLiteral("not null"));
        QCOMPARE(columns.rows[0][5], QStringLiteral("always")); // Identity.
        QCOMPARE(columns.rows[1][6], QStringLiteral("What it is called"));
        QCOMPARE(columns.rows[3][2], QStringLiteral("numeric(10,2)"));
        QCOMPARE(columns.rows[3][4], QStringLiteral("0"));

        const DetailTable indexes = table(detail, QStringLiteral("Indexes"));
        QCOMPARE(indexes.rows.size(), 2u);
        QVERIFY(firstColumn(indexes).contains(QStringLiteral("t_name")));
        QVERIFY(indexes.rows[0][3].startsWith(QLatin1String("CREATE")));

        // A primary key, a foreign key and a check constraint.
        const QStringList constraints = firstColumn(table(detail, QStringLiteral("Constraints")));
        QCOMPARE(constraints.size(), 3);
        QCOMPARE(firstColumn(table(detail, QStringLiteral("Triggers"))),
                 QStringList {QStringLiteral("t_trg")});
        QVERIFY(detail.definition.isEmpty()); // A table has no definition query.
    }

    void view()
    {
        const ObjectDetail detail = detailOf(ObjectKind::View, oidOf("'slonisko_details_test.v'"));
        QCOMPARE(detail.title, QStringLiteral("slonisko_details_test.v"));
        QCOMPARE(firstColumn(table(detail, QStringLiteral("Columns")), 1),
                 (QStringList {QStringLiteral("id"), QStringLiteral("name")}));
        QVERIFY2(detail.definition.contains(QLatin1String("amount")),
                 qPrintable(detail.definition));
        // A plain view has no indexes, so it gets no such tab.
        QVERIFY(std::ranges::none_of(detail.tables, [](const DetailTable &t) {
            return t.title == QLatin1String("Indexes");
        }));
    }

    void materializedView()
    {
        const ObjectDetail detail
            = detailOf(ObjectKind::MaterializedView, oidOf("'slonisko_details_test.mv'"));
        QCOMPARE(firstColumn(table(detail, QStringLiteral("Indexes"))),
                 QStringList {QStringLiteral("mv_id")});
        QVERIFY(!detail.definition.isEmpty());
    }

    void extension()
    {
        const ObjectDetail detail
            = detailOf(ObjectKind::Extension,
                       oidFrom("SELECT oid FROM pg_extension WHERE extname = 'plpgsql'"));
        QCOMPARE(detail.title, QStringLiteral("plpgsql"));
        QCOMPARE(detail.subtitle, kindName(ObjectKind::Extension));
        QVERIFY(!property(detail, QStringLiteral("version")).isEmpty());
        QCOMPARE(property(detail, QStringLiteral("schema")), QStringLiteral("pg_catalog"));
        QCOMPARE(property(detail, QStringLiteral("relocatable")), QStringLiteral("no"));
        QVERIFY(
            property(detail, QStringLiteral("description")).contains(QLatin1String("PL/pgSQL")));

        // What it brought with it: the language's handler functions.
        const DetailTable objects = table(detail, QStringLiteral("Objects"));
        QVERIFY(!objects.rows.empty());
        QVERIFY(firstColumn(objects, 1).contains(QStringLiteral("plpgsql_call_handler")));
    }

    // Every kind's queries have to be valid SQL and find their object; this
    // is what catches a typo or a column that a PostgreSQL version renamed.
    void everyKind_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QByteArray>("oidQuery");
        QTest::addColumn<QString>("title");

        auto row = [](const char *name, ObjectKind kind, const char *oidQuery, const char *title) {
            QTest::newRow(name) << int(kind) << QByteArray(oidQuery) << QString::fromUtf8(title);
        };
        row("database", ObjectKind::Database,
            "SELECT oid FROM pg_database WHERE datname = current_database()", "");
        row("schema", ObjectKind::Schema,
            "SELECT oid FROM pg_namespace WHERE nspname = 'slonisko_details_test'",
            "slonisko_details_test");
        row("sequence", ObjectKind::Sequence, "SELECT 'slonisko_details_test.s'::regclass::oid",
            "slonisko_details_test.s");
        row("function", ObjectKind::Function,
            "SELECT 'slonisko_details_test.add(int, int)'::regprocedure::oid",
            "slonisko_details_test.add");
        row("procedure", ObjectKind::Procedure,
            "SELECT 'slonisko_details_test.p(int)'::regprocedure::oid", "slonisko_details_test.p");
        row("aggregate", ObjectKind::Aggregate,
            "SELECT 'slonisko_details_test.mysum(int)'::regprocedure::oid",
            "slonisko_details_test.mysum");
        row("enum type", ObjectKind::Type, "SELECT 'slonisko_details_test.mood'::regtype::oid",
            "slonisko_details_test.mood");
        row("composite type", ObjectKind::Type, "SELECT 'slonisko_details_test.pair'::regtype::oid",
            "slonisko_details_test.pair");
        row("domain", ObjectKind::Domain, "SELECT 'slonisko_details_test.posint'::regtype::oid",
            "slonisko_details_test.posint");
        row("index", ObjectKind::Index, "SELECT 'slonisko_details_test.t_name'::regclass::oid",
            "slonisko_details_test.t_name");
        row("trigger", ObjectKind::Trigger, "SELECT oid FROM pg_trigger WHERE tgname = 't_trg'",
            "t_trg");
        row("policy", ObjectKind::Policy, "SELECT oid FROM pg_policy WHERE polname = 't_read'",
            "t_read");
        row("constraint", ObjectKind::Constraint,
            "SELECT oid FROM pg_constraint WHERE conname = 't_pkey'", "t_pkey");
        row("publication", ObjectKind::Publication,
            "SELECT oid FROM pg_publication WHERE pubname = 'slonisko_details_pub'",
            "slonisko_details_pub");
        row("event trigger", ObjectKind::EventTrigger,
            "SELECT oid FROM pg_event_trigger WHERE evtname = 'slonisko_details_evt'",
            "slonisko_details_evt");
        row("role", ObjectKind::Role, "SELECT oid FROM pg_roles WHERE rolname = current_user", "");
        row("tablespace", ObjectKind::Tablespace,
            "SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default'", "pg_default");
    }

    void everyKind()
    {
        QFETCH(int, kind);
        QFETCH(QByteArray, oidQuery);
        QFETCH(QString, title);

        const Oid oid = oidFrom(oidQuery);
        QVERIFY2(oid > 0, oidQuery.constData());
        const ObjectDetail detail = detailOf(ObjectKind(kind), oid);
        QVERIFY(!detail.title.isEmpty());
        if (!title.isEmpty())
            QCOMPARE(detail.title, title);
        QVERIFY(!detail.properties.rows.empty());
    }

    void sequence()
    {
        const ObjectDetail detail
            = detailOf(ObjectKind::Sequence, oidOf("'slonisko_details_test.s'"));
        QCOMPARE(property(detail, QStringLiteral("start")), QStringLiteral("5"));
        QCOMPARE(property(detail, QStringLiteral("increment")), QStringLiteral("2"));
        QCOMPARE(property(detail, QStringLiteral("cycles")), QStringLiteral("no"));
        QCOMPARE(property(detail, QStringLiteral("type")), QStringLiteral("bigint"));
    }

    void functionShowsItsSource()
    {
        const ObjectDetail detail
            = detailOf(ObjectKind::Function,
                       oidFrom("SELECT 'slonisko_details_test.add(int, int)'::regprocedure::oid"));
        QCOMPARE(property(detail, QStringLiteral("arguments")),
                 QStringLiteral("a integer, b integer"));
        QCOMPARE(property(detail, QStringLiteral("returns")), QStringLiteral("integer"));
        QCOMPARE(property(detail, QStringLiteral("language")), QStringLiteral("sql"));
        QVERIFY2(detail.definition.contains(QLatin1String("a + b")), qPrintable(detail.definition));
    }

    void aggregateShowsItsSupportFunctions()
    {
        const ObjectDetail detail
            = detailOf(ObjectKind::Aggregate,
                       oidFrom("SELECT 'slonisko_details_test.mysum(int)'::regprocedure::oid"));
        QVERIFY(detail.definition.isEmpty()); // There is no source to show.
        const DetailTable support = table(detail, QStringLiteral("Support"));
        QCOMPARE(firstColumn(support, 1).value(0), QStringLiteral("int4pl"));
    }

    void typesAndDomains()
    {
        const ObjectDetail mood = detailOf(
            ObjectKind::Type, oidFrom("SELECT 'slonisko_details_test.mood'::regtype::oid"));
        QCOMPARE(property(mood, QStringLiteral("type")), QStringLiteral("enum"));
        QCOMPARE(firstColumn(table(mood, QStringLiteral("Values"))),
                 (QStringList {QStringLiteral("sad"), QStringLiteral("happy")}));

        const ObjectDetail pair = detailOf(
            ObjectKind::Type, oidFrom("SELECT 'slonisko_details_test.pair'::regtype::oid"));
        QCOMPARE(property(pair, QStringLiteral("type")), QStringLiteral("composite"));
        QCOMPARE(firstColumn(table(pair, QStringLiteral("Attributes")), 1),
                 (QStringList {QStringLiteral("a"), QStringLiteral("b")}));

        const ObjectDetail domain = detailOf(
            ObjectKind::Domain, oidFrom("SELECT 'slonisko_details_test.posint'::regtype::oid"));
        QCOMPARE(property(domain, QStringLiteral("base type")), QStringLiteral("integer"));
        QCOMPARE(property(domain, QStringLiteral("nullable")), QStringLiteral("not null"));
        QVERIFY(!table(domain, QStringLiteral("Constraints")).rows.empty());
    }

    void indexAndTrigger()
    {
        const ObjectDetail index
            = detailOf(ObjectKind::Index, oidOf("'slonisko_details_test.t_name'"));
        QCOMPARE(property(index, QStringLiteral("table")), QStringLiteral("t"));
        QCOMPARE(property(index, QStringLiteral("method")), QStringLiteral("btree"));
        QCOMPARE(property(index, QStringLiteral("kind")), QStringLiteral("non-unique"));
        QVERIFY(index.definition.startsWith(QLatin1String("CREATE INDEX")));
        QCOMPARE(firstColumn(table(index, QStringLiteral("Columns")), 1),
                 QStringList {QStringLiteral("name")});

        const ObjectDetail trigger = detailOf(
            ObjectKind::Trigger, oidFrom("SELECT oid FROM pg_trigger WHERE tgname = 't_trg'"));
        QCOMPARE(property(trigger, QStringLiteral("timing")), QStringLiteral("before"));
        QCOMPARE(property(trigger, QStringLiteral("events")), QStringLiteral("insert"));
        QCOMPARE(property(trigger, QStringLiteral("for each")), QStringLiteral("row"));
        QVERIFY(trigger.definition.contains(QLatin1String("EXECUTE FUNCTION")));
    }

    void schemaCountsItsContents()
    {
        const ObjectDetail detail = detailOf(
            ObjectKind::Schema,
            oidFrom("SELECT oid FROM pg_namespace WHERE nspname = 'slonisko_details_test'"));
        const DetailTable contents = table(detail, QStringLiteral("Contents"));
        QVERIFY(!contents.rows.empty());
        const QStringList kinds = firstColumn(contents);
        QVERIFY2(kinds.contains(QStringLiteral("tables")), qPrintable(kinds.join(u',')));
        QVERIFY(kinds.contains(QStringLiteral("views")));
        QVERIFY(kinds.contains(QStringLiteral("functions")));
    }

    void tableShowsItsPolicies()
    {
        const ObjectDetail detail = detailOf(ObjectKind::Table, oidOf("'slonisko_details_test.t'"));
        QCOMPARE(property(detail, QStringLiteral("row security")), QStringLiteral("enabled"));
        QCOMPARE(firstColumn(table(detail, QStringLiteral("Policies"))),
                 QStringList {QStringLiteral("t_read")});
    }

    // The tree has no oid for the database a connection was made to.
    void databaseWithoutAnOid()
    {
        const ObjectDetail detail = detailOf(ObjectKind::Database, 0);
        QVERIFY(!detail.title.isEmpty());
        QVERIFY(!property(detail, QStringLiteral("encoding")).isEmpty());
        QCOMPARE(detail.title,
                 QString::fromUtf8(run("SELECT current_database()").results[0].value(0, 0)));
    }

    void objectThatIsGone()
    {
        const ObjectDetail detail = detailOf(ObjectKind::Table, 1); // No such relation.
        QVERIFY(detail.title.isEmpty());
        QVERIFY(detail.properties.rows.empty());
        QVERIFY(detail.tables.empty());
    }

private:
    QueryOutcome run(const QByteArray &sql)
    {
        slonisko::pg::QueryRunner runner(&m_connection);
        QueryOutcome outcome;
        bool done = false;
        runner.run(sql, this, [&](const QueryOutcome &result) {
            outcome = result;
            done = true;
        });
        [&] { QVERIFY(QTest::qWaitFor([&] { return done; }, 15'000)); }();
        return outcome;
    }

    // A regclass literal, like "'public.t'".
    Oid oidOf(const QByteArray &regclass)
    {
        return oidFrom("SELECT " + regclass + "::regclass::oid");
    }

    // A complete query returning one oid.
    Oid oidFrom(const QByteArray &query)
    {
        const QueryOutcome o = run(query);
        return o.ok() && !o.results.empty() && o.results[0].rowCount() > 0
            ? o.results[0].value(0, 0).toUInt()
            : 0;
    }

    ObjectDetail detailOf(ObjectKind kind, Oid oid)
    {
        std::vector<Result> results;
        for (const QByteArray &query : detailQueries(kind, oid)) {
            const QueryOutcome o = run(query);
            [&] {
                QVERIFY2(o.ok(),
                         qPrintable(o.error + QLatin1String(": ") + QString::fromUtf8(query)));
            }();
            results.push_back(o.results.empty() ? Result() : o.results.back());
        }
        return parseDetail(kind, results);
    }

    static QString property(const ObjectDetail &detail, const QString &name)
    {
        for (const QStringList &row : detail.properties.rows) {
            if (row.value(0) == name)
                return row.value(1);
        }
        return {};
    }

    static DetailTable table(const ObjectDetail &detail, const QString &title)
    {
        for (const DetailTable &table : detail.tables) {
            if (table.title == title)
                return table;
        }
        return {};
    }

    static QStringList firstColumn(const DetailTable &table, int column = 0)
    {
        QStringList out;
        for (const QStringList &row : table.rows)
            out << row.value(column);
        return out;
    }

    Connection m_connection;
};

QTEST_MAIN(TestDetails)
#include "tst_details.moc"
