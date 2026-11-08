// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Dependencies.h"
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
DROP SCHEMA IF EXISTS slonisko_deps_test CASCADE;
DROP SCHEMA IF EXISTS slonisko_deps_other CASCADE;
CREATE SCHEMA slonisko_deps_test;
CREATE SCHEMA slonisko_deps_other;
SET search_path = slonisko_deps_test;
CREATE TABLE p (id int PRIMARY KEY, name text, kind int);
CREATE SEQUENCE s;
CREATE TABLE c (id int DEFAULT nextval('s'), p_id int REFERENCES p);
CREATE INDEX c_p_id ON c (p_id);
CREATE VIEW v AS SELECT id, name FROM p;
CREATE VIEW vv AS SELECT name FROM v;
ALTER TABLE c ENABLE ROW LEVEL SECURITY;
CREATE POLICY pol ON c USING (p_id IN (SELECT id FROM p));
CREATE FUNCTION counted() RETURNS bigint LANGUAGE sql RETURN (SELECT count(*) FROM p);
CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER t BEFORE UPDATE OF name ON p FOR EACH ROW EXECUTE FUNCTION trg();
CREATE TYPE ct AS (a int);
CREATE TABLE uses_ct (x ct);
-- Only the bodies say what these need.
CREATE FUNCTION report() RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
    RETURN (SELECT count(*) FROM p);
END $$;
CREATE FUNCTION kinds() RETURNS SETOF int LANGUAGE sql AS 'SELECT kind FROM p';
CREATE VIEW rv AS SELECT report() AS n;
-- Unqualified, and run with a search path of its own.
CREATE TABLE slonisko_deps_other.p (id int);
CREATE FUNCTION elsewhere() RETURNS bigint LANGUAGE sql
    SET search_path = slonisko_deps_other AS 'SELECT count(*) FROM p';
CREATE TABLE parted (id int, k int) PARTITION BY RANGE (k);
CREATE TABLE parted_1 PARTITION OF parted FOR VALUES FROM (0) TO (10);
CREATE TABLE parent (id int);
CREATE TABLE child () INHERITS (parent);
)sql";

const char *const Teardown = R"sql(
DROP SCHEMA IF EXISTS slonisko_deps_test CASCADE;
DROP SCHEMA IF EXISTS slonisko_deps_other CASCADE;
)sql";

} // namespace

// What a DROP or a type change would hit: pg_depend's dependents and what
// function bodies name, followed as far as they go.
class TestDependenciesDb : public QObject
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
        // The bodies are looked up in the connection's search path.
        QVERIFY(run("SET search_path = slonisko_deps_test, public").ok());
    }

    void cleanupTestCase()
    {
        if (m_connection.state() == Connection::State::Ready)
            run(Teardown);
    }

    void droppingATable()
    {
        const DependencyGraph graph = graphOf(ObjectKind::Table, oidOf("'p'"));
        QVERIFY(graph.unreadable.isEmpty());
        QCOMPARE(
            graph.rootColumns,
            QStringList({QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("kind")}));
        const Impact impact = impactOf(graph);

        const Dependent *view = find(graph, impact, "slonisko_deps_test.v");
        QVERIFY(view);
        QCOMPARE(view->depth, 1);
        QCOMPARE(view->effect, DropEffect::Cascade);
        QCOMPARE(view->columns, QStringList({QStringLiteral("id"), QStringLiteral("name")}));
        QCOMPARE(objectKindOf(graph.objects[std::size_t(view->object)]), ObjectKind::View);
        // A view on the view goes too, under it.
        const Dependent *onView = find(graph, impact, "slonisko_deps_test.vv");
        QVERIFY(onView);
        QCOMPARE(onView->depth, 2);
        QCOMPARE(&impact.dependents[std::size_t(onView->parent)], view);

        const Dependent *fk = find(graph, impact, "c_p_id_fkey on slonisko_deps_test.c");
        QVERIFY(fk);
        QCOMPARE(fk->reason, QStringLiteral("foreign key references it"));
        QCOMPARE(fk->effect, DropEffect::Cascade);
        // Its own pieces are part of it: the primary key's index and the
        // foreign key's triggers are not listed apart.
        QVERIFY(find(graph, impact, "p_pkey on slonisko_deps_test.p"));
        QVERIFY(!find(graph, impact, "slonisko_deps_test.p_pkey"));
        QVERIFY(std::ranges::none_of(impact.dependents, [&](const Dependent &d) {
            return graph.objects[std::size_t(d.object)].identity.contains(
                QLatin1String("RI_ConstraintTrigger"));
        }));
        // Nor are its row type, its TOAST table or its NOT NULL constraint.
        QVERIFY(!find(graph, impact, "p_id_not_null on slonisko_deps_test.p"));
        QVERIFY(std::ranges::none_of(impact.dependents, [&](const Dependent &d) {
            return graph.objects[std::size_t(d.object)].type == QLatin1String("type")
                || graph.objects[std::size_t(d.object)].schema == QLatin1String("pg_toast");
        }));

        // On the table and on one of its columns: it goes along all the same.
        const Dependent *trigger = find(graph, impact, "t on slonisko_deps_test.p");
        QVERIFY(trigger);
        QCOMPARE(trigger->effect, DropEffect::DroppedAlong);
        QCOMPARE(trigger->reason, QStringLiteral("trigger on it"));
        const Dependent *policy = find(graph, impact, "pol on slonisko_deps_test.c");
        QVERIFY(policy);
        QCOMPARE(policy->effect, DropEffect::Cascade);
        // A body in standard SQL is tracked by the server.
        const Dependent *counted = find(graph, impact, "slonisko_deps_test.counted()");
        QVERIFY(counted);
        QCOMPARE(counted->effect, DropEffect::Cascade);

        // One in a string, and PL/pgSQL, only by name: they stay, and fail.
        const Dependent *report = find(graph, impact, "slonisko_deps_test.report()");
        QVERIFY(report);
        QCOMPARE(report->effect, DropEffect::FailsWhenRun);
        QCOMPARE(report->reason, QStringLiteral("names it in its body"));
        QVERIFY(find(graph, impact, "slonisko_deps_test.kinds()"));
        // And so does what uses them, though the server knows that much.
        const Dependent *rv = find(graph, impact, "slonisko_deps_test.rv");
        QVERIFY(rv);
        QCOMPARE(rv->effect, DropEffect::FailsWhenRun);
        QCOMPARE(rv->depth, 2);
        // That one's p is another schema's.
        QVERIFY(!find(graph, impact, "slonisko_deps_test.elsewhere()"));

        QCOMPARE(impact.failing, 3);
        QVERIFY(impact.cascade >= 5);
        QVERIFY(impact.dropped > impact.cascade);
        const QString summary = impactSummary(graph, impact, false);
        QVERIFY2(summary.startsWith(QLatin1String("A plain DROP is refused: ")),
                 qPrintable(summary));
        QVERIFY2(summary.contains(QLatin1String("3 more only name it in function bodies")),
                 qPrintable(summary));
        // Parents always come before their children.
        for (std::size_t i = 0; i < impact.dependents.size(); ++i)
            QVERIFY(impact.dependents[i].parent < int(i));
    }

    void searchPathOfItsOwn()
    {
        const DependencyGraph graph = graphOf(ObjectKind::Table, oidOf("'slonisko_deps_other.p'"));
        const Impact impact = impactOf(graph);
        QVERIFY(find(graph, impact, "slonisko_deps_test.elsewhere()"));
        QVERIFY(!find(graph, impact, "slonisko_deps_test.report()"));
    }

    void changingAColumnsType()
    {
        const DependencyGraph graph = graphOf(ObjectKind::Table, oidOf("'p'"));

        const Impact name = impactOf(graph, QStringLiteral("name"));
        QVERIFY(find(graph, name, "slonisko_deps_test.v"));
        QVERIFY(find(graph, name, "slonisko_deps_test.vv"));
        const Dependent *trigger = find(graph, name, "t on slonisko_deps_test.p");
        QVERIFY(trigger);
        QCOMPARE(trigger->reason, QStringLiteral("trigger fires on update of it"));
        QVERIFY(!find(graph, name, "c_p_id_fkey on slonisko_deps_test.c"));
        QVERIFY(!find(graph, name, "slonisko_deps_test.kinds()"));

        const Impact kind = impactOf(graph, QStringLiteral("kind"));
        QVERIFY(!find(graph, kind, "slonisko_deps_test.v"));
        QVERIFY(!find(graph, kind, "t on slonisko_deps_test.p"));
        // Only the body that mentions the column; count(*) does not.
        QVERIFY(find(graph, kind, "slonisko_deps_test.kinds()"));
        QVERIFY(!find(graph, kind, "slonisko_deps_test.report()"));

        const Impact id = impactOf(graph, QStringLiteral("id"));
        QVERIFY(find(graph, id, "c_p_id_fkey on slonisko_deps_test.c"));
        QVERIFY(find(graph, id, "pol on slonisko_deps_test.c"));
        QVERIFY(find(graph, id, "p_pkey on slonisko_deps_test.p"));
    }

    void droppingASequence()
    {
        const DependencyGraph graph = graphOf(ObjectKind::Sequence, oidOf("'s'"));
        const Impact impact = impactOf(graph);
        const Dependent *table = find(graph, impact, "slonisko_deps_test.c");
        QVERIFY(table);
        QCOMPARE(table->reason, QStringLiteral("default of column id uses it"));
    }

    void droppingATypeOrAFunction()
    {
        const DependencyGraph type
            = graphOf(ObjectKind::Type, oidFrom("SELECT 'ct'::regtype::oid"));
        const Dependent *column = find(type, impactOf(type), "slonisko_deps_test.uses_ct");
        QVERIFY(column);
        QCOMPARE(column->reason, QStringLiteral("column x is of this type"));

        const DependencyGraph function
            = graphOf(ObjectKind::Function, oidFrom("SELECT 'trg'::regproc::oid"));
        const Dependent *trigger = find(function, impactOf(function), "t on slonisko_deps_test.p");
        QVERIFY(trigger);
        QCOMPARE(trigger->reason, QStringLiteral("trigger executes it"));
        QCOMPARE(trigger->effect, DropEffect::Cascade);

        const DependencyGraph report
            = graphOf(ObjectKind::Function, oidFrom("SELECT 'report'::regproc::oid"));
        QVERIFY(find(report, impactOf(report), "slonisko_deps_test.rv"));
    }

    // Partitions go with their table; inheriting tables only by CASCADE.
    // Either has every column of it, so a type change reaches them too.
    void partitionsAndInheritance()
    {
        const DependencyGraph parted = graphOf(ObjectKind::PartitionedTable, oidOf("'parted'"));
        const Dependent *partition = find(parted, impactOf(parted), "slonisko_deps_test.parted_1");
        QVERIFY(partition);
        QCOMPARE(partition->reason, QStringLiteral("partition of it"));
        QCOMPARE(partition->effect, DropEffect::DroppedAlong);
        QVERIFY(
            find(parted, impactOf(parted, QStringLiteral("id")), "slonisko_deps_test.parted_1"));

        const DependencyGraph parent = graphOf(ObjectKind::Table, oidOf("'parent'"));
        const Dependent *child = find(parent, impactOf(parent), "slonisko_deps_test.child");
        QVERIFY(child);
        QCOMPARE(child->reason, QStringLiteral("inherits from it"));
        QCOMPARE(child->effect, DropEffect::Cascade);
        QVERIFY(find(parent, impactOf(parent, QStringLiteral("id")), "slonisko_deps_test.child"));
    }

    void nothingDependsOnIt()
    {
        const DependencyGraph graph = graphOf(ObjectKind::View, oidOf("'vv'"));
        QVERIFY(graph.root >= 0);
        const Impact impact = impactOf(graph);
        QVERIFY(impact.dependents.empty());
        QCOMPARE(impactSummary(graph, impact, false), QStringLiteral("Nothing depends on it."));
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

    Oid oidOf(const QByteArray &regclass)
    {
        return oidFrom("SELECT " + regclass + "::regclass::oid");
    }

    Oid oidFrom(const QByteArray &query)
    {
        const QueryOutcome o = run(query);
        return o.ok() && !o.results.empty() && o.results[0].rowCount() > 0
            ? o.results[0].value(0, 0).toUInt()
            : 0;
    }

    DependencyGraph graphOf(ObjectKind kind, Oid oid)
    {
        std::vector<Result> results;
        for (const QByteArray &query : dependencyQueries(kind, oid)) {
            const QueryOutcome o = run(query);
            [&] {
                QVERIFY2(o.ok(),
                         qPrintable(o.error + QLatin1String(": ") + QString::fromUtf8(query)));
            }();
            results.push_back(o.results.empty() ? Result() : o.results.back());
        }
        return parseDependencies(kind, oid, results);
    }

    static const Dependent *find(const DependencyGraph &graph, const Impact &impact,
                                 const char *identity)
    {
        for (const Dependent &d : impact.dependents) {
            if (graph.objects[std::size_t(d.object)].identity == QLatin1String(identity))
                return &d;
        }
        return nullptr;
    }

    Connection m_connection;
};

QTEST_MAIN(TestDependenciesDb)
#include "tst_dependenciesdb.moc"
