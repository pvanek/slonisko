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
)sql";

const char *const Teardown = "DROP SCHEMA IF EXISTS slonisko_details_test CASCADE";

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
        QVERIFY(!hasDetails(ObjectKind::Sequence));
        QVERIFY(!hasDetails(ObjectKind::Schema));
        QVERIFY(detailQueries(ObjectKind::Sequence, 1).empty());
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

        const DetailTable &columns = table(detail, QStringLiteral("Columns"));
        QCOMPARE(columns.rows.size(), 4u);
        QCOMPARE(columns.rows[0][1], QStringLiteral("id"));
        QCOMPARE(columns.rows[0][2], QStringLiteral("bigint"));
        QCOMPARE(columns.rows[0][3], QStringLiteral("not null"));
        QCOMPARE(columns.rows[0][5], QStringLiteral("always")); // Identity.
        QCOMPARE(columns.rows[1][6], QStringLiteral("What it is called"));
        QCOMPARE(columns.rows[3][2], QStringLiteral("numeric(10,2)"));
        QCOMPARE(columns.rows[3][4], QStringLiteral("0"));

        const DetailTable &indexes = table(detail, QStringLiteral("Indexes"));
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
        const ObjectDetail detail = detailOf(
            ObjectKind::Extension, oidOf("oid FROM pg_extension WHERE extname = 'plpgsql'", true));
        QCOMPARE(detail.title, QStringLiteral("plpgsql"));
        QCOMPARE(detail.subtitle, kindName(ObjectKind::Extension));
        QVERIFY(!property(detail, QStringLiteral("version")).isEmpty());
        QCOMPARE(property(detail, QStringLiteral("schema")), QStringLiteral("pg_catalog"));
        QCOMPARE(property(detail, QStringLiteral("relocatable")), QStringLiteral("no"));
        QVERIFY(
            property(detail, QStringLiteral("description")).contains(QLatin1String("PL/pgSQL")));

        // What it brought with it: the language's handler functions.
        const DetailTable &objects = table(detail, QStringLiteral("Objects"));
        QVERIFY(!objects.rows.empty());
        QVERIFY(firstColumn(objects, 1).contains(QStringLiteral("plpgsql_call_handler")));
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

    // Either a regclass literal, or a whole "column FROM ..." tail.
    Oid oidOf(const QByteArray &what, bool tail = false)
    {
        const QueryOutcome o = run(tail ? "SELECT " + what : "SELECT " + what + "::regclass::oid");
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

    static const DetailTable &table(const ObjectDetail &detail, const QString &title)
    {
        static const DetailTable none;
        for (const DetailTable &table : detail.tables) {
            if (table.title == title)
                return table;
        }
        return none;
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
