// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Erd.h"
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
DROP SCHEMA IF EXISTS slonisko_erd_test CASCADE;
CREATE SCHEMA slonisko_erd_test;
SET search_path = slonisko_erd_test;
CREATE TABLE customer (id int PRIMARY KEY, name text NOT NULL);
CREATE TABLE country (code char(2) PRIMARY KEY, name text);
CREATE TABLE orders (
    id int PRIMARY KEY,
    customer_id int NOT NULL REFERENCES customer (id),
    country_code char(2) REFERENCES country (code),
    parent_id int REFERENCES orders (id),
    total numeric(10, 2));
CREATE TABLE order_line (
    order_id int REFERENCES orders (id),
    line int,
    PRIMARY KEY (order_id, line));
CREATE TABLE shipment (
    order_id int,
    line int,
    FOREIGN KEY (order_id, line) REFERENCES order_line (order_id, line));
CREATE TABLE lonely (id int PRIMARY KEY);
)sql";

const char *const Teardown = "DROP SCHEMA IF EXISTS slonisko_erd_test CASCADE";

// A graph built by hand, so the layout can be tested without a server.
ErdNode makeNode(unsigned int oid, const char *name, int columns)
{
    ErdNode node;
    node.oid = oid;
    node.schema = QStringLiteral("public");
    node.name = QString::fromUtf8(name);
    for (int i = 0; i < columns; ++i)
        node.columns.push_back({QStringLiteral("c%1").arg(i), QStringLiteral("integer")});
    return node;
}

ErdEdge makeEdge(const char *name, unsigned int from, unsigned int to)
{
    ErdEdge edge;
    edge.name = QString::fromUtf8(name);
    edge.from = from;
    edge.to = to;
    edge.fromColumns = QStringList {QStringLiteral("x")};
    edge.toColumns = QStringList {QStringLiteral("id")};
    return edge;
}

bool overlap(const ErdLayout &layout)
{
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < layout.nodes.size(); ++j) {
            if (layout.nodes[i].box.intersects(layout.nodes[j].box))
                return true;
        }
    }
    return false;
}

} // namespace

class TestErd : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // The layout: no server needed, so every odd shape can be tried.

    void starPutsReferencedAboveAndReferencingBelow()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes
            = {makeNode(1, "orders", 5), makeNode(2, "customer", 3), makeNode(3, "order_line", 2)};
        graph.edges
            = {makeEdge("orders_customer_fkey", 1, 2), makeEdge("order_line_orders_fkey", 3, 1)};

        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 3u);
        const ErdPlacement *focus = layout.placement(1);
        const ErdPlacement *referenced = layout.placement(2);
        const ErdPlacement *referencing = layout.placement(3);
        QVERIFY(focus && referenced && referencing);

        QVERIFY(referenced->box.bottom() < focus->box.top());
        QVERIFY(focus->box.bottom() < referencing->box.top());
        // The focus table is in the middle of the picture.
        QVERIFY(qAbs(focus->box.center().x() - layout.bounds.center().x()) < 1.0);
        QVERIFY(!overlap(layout));
        QVERIFY(layout.bounds.contains(focus->box));
    }

    void selfReferenceNeedsNoPlace()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes = {makeNode(1, "orders", 4)};
        graph.edges = {makeEdge("orders_parent_fkey", 1, 1)};

        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 1u);
        QCOMPARE(layout.placement(1)->box.topLeft(), QPointF(0, 0));
        QVERIFY(graph.edges.front().isSelfReference());
    }

    void severalKeysToTheSameTableArePlacedOnce()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes = {makeNode(1, "orders", 4), makeNode(2, "customer", 2)};
        graph.edges = {makeEdge("orders_buyer_fkey", 1, 2), makeEdge("orders_payer_fkey", 1, 2)};

        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 2u);
        QVERIFY(!overlap(layout));
    }

    void tablesWithNoKeysStillGetAPlace()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes = {makeNode(1, "orders", 2), makeNode(9, "lonely", 1)};

        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 2u);
        QVERIFY(!overlap(layout));
    }

    void layoutIsDeterministic()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes = {makeNode(1, "orders", 3), makeNode(2, "customer", 2),
                       makeNode(3, "country", 2), makeNode(4, "currency", 2)};
        graph.edges
            = {makeEdge("a_fkey", 1, 2), makeEdge("b_fkey", 1, 3), makeEdge("c_fkey", 1, 4)};

        const ErdLayout first = starLayout(graph);
        // The same graph, listed the other way round, lays out the same.
        std::ranges::reverse(graph.nodes);
        std::ranges::reverse(graph.edges);
        const ErdLayout second = starLayout(graph);
        QCOMPARE(first.nodes.size(), second.nodes.size());
        for (const ErdPlacement &placement : first.nodes)
            QCOMPARE(second.placement(placement.oid)->box, placement.box);
        // Neighbours sit in the order of their names, not of their oids.
        QVERIFY(first.placement(3)->box.left() < first.placement(4)->box.left());
        QVERIFY(first.placement(4)->box.left() < first.placement(2)->box.left());
    }

    void emptyGraphLaysOutToNothing()
    {
        const ErdLayout layout = starLayout({});
        QVERIFY(layout.nodes.empty());
        QVERIFY(layout.bounds.isNull());
    }

    void wideTablesAreCutToSize()
    {
        ErdNode node = makeNode(1, "wide", 40);
        node.otherColumns = 5;
        node.columns[0].name = QString(200, QLatin1Char('x'));
        ErdMetrics metrics;
        metrics.maxRows = 10;
        const QSizeF size = metrics.sizeOf(node);
        QCOMPARE(size.width(), metrics.maxWidth);
        // Ten rows and the line counting what is not shown.
        QCOMPARE(size.height(), metrics.headerHeight + 11 * metrics.rowHeight + metrics.padding);

        // Keys only and none of them cut: still a line for the other columns.
        ErdNode slim = makeNode(2, "slim", 2);
        slim.otherColumns = 9;
        QCOMPARE(ErdMetrics {}.sizeOf(slim).height(),
                 ErdMetrics {}.headerHeight + 3 * ErdMetrics {}.rowHeight + ErdMetrics {}.padding);
    }

    // The queries, against a real server.

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

    void graphOfOneTableAndItsNeighbours()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.orders");
        QCOMPARE(graph.focus, oidOf("slonisko_erd_test.orders"));

        QStringList names;
        for (const ErdNode &node : graph.nodes)
            names << node.name;
        // The table itself, the two it references, the one referencing it,
        // and nothing two hops away (shipment references order_line only).
        QCOMPARE(names.first(), QStringLiteral("orders")); // The focus goes first.
        QCOMPARE(names.size(), 4);
        QVERIFY(names.contains(QStringLiteral("customer")));
        QVERIFY(names.contains(QStringLiteral("country")));
        QVERIFY(names.contains(QStringLiteral("order_line")));
        QVERIFY(!names.contains(QStringLiteral("shipment")));
        QVERIFY(!names.contains(QStringLiteral("lonely")));

        // Only the key columns: id, customer_id, country_code, parent_id.
        // total is not part of any key, so it is counted, not listed.
        const ErdNode *orders = graph.node(graph.focus);
        QVERIFY(orders);
        QCOMPARE(orders->qualifiedName(), QStringLiteral("slonisko_erd_test.orders"));
        QCOMPARE(orders->columns.size(), 4u);
        QCOMPARE(orders->otherColumns, 1);
        QCOMPARE(orders->columns[0].name, QStringLiteral("id"));
        QVERIFY(orders->columns[0].primaryKey);
        QCOMPARE(orders->columns[1].name, QStringLiteral("customer_id"));
        QVERIFY(orders->columns[1].foreignKey);
        QVERIFY(orders->columns[1].notNull);
        QCOMPARE(orders->columns[3].name, QStringLiteral("parent_id"));
        QVERIFY(std::ranges::none_of(
            orders->columns, [](const ErdColumn &c) { return c.name == QLatin1String("total"); }));

        // A neighbour keeps only its keys too.
        const ErdNode *customer = graph.node(oidOf("slonisko_erd_test.customer"));
        QVERIFY(customer);
        QCOMPARE(customer->columns.size(), 1u); // id
        QCOMPARE(customer->otherColumns, 1); // name
    }

    void edgesCarryTheirColumns()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.orders");
        const Oid orders = oidOf("slonisko_erd_test.orders");
        const Oid customer = oidOf("slonisko_erd_test.customer");

        const auto customerEdge = std::ranges::find_if(
            graph.edges, [&](const ErdEdge &e) { return e.from == orders && e.to == customer; });
        QVERIFY(customerEdge != graph.edges.end());
        QCOMPARE(customerEdge->fromColumns, QStringList {QStringLiteral("customer_id")});
        QCOMPARE(customerEdge->toColumns, QStringList {QStringLiteral("id")});

        // The key pointing at the table itself is there, marked as such.
        QCOMPARE(std::ranges::count_if(graph.edges, &ErdEdge::isSelfReference), 1);
    }

    void compositeKeysKeepTheirOrder()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.shipment");
        QCOMPARE(graph.edges.size(), 1u);
        const ErdEdge &edge = graph.edges.front();
        QCOMPARE(edge.fromColumns,
                 (QStringList {QStringLiteral("order_id"), QStringLiteral("line")}));
        QCOMPARE(edge.toColumns,
                 (QStringList {QStringLiteral("order_id"), QStringLiteral("line")}));
    }

    // A table with no keys at all must still turn up, or its diagram would
    // be empty rather than a box on its own.
    void tableWithNoKeyColumns()
    {
        const QueryOutcome created = run("CREATE TABLE slonisko_erd_test.keyless (a int, b text)");
        QVERIFY2(created.ok(), qPrintable(created.error));
        const ErdGraph graph = graphOf("slonisko_erd_test.keyless");
        QCOMPARE(graph.nodes.size(), 1u);
        const ErdNode &node = graph.nodes.front();
        QVERIFY(node.columns.empty());
        QCOMPARE(node.otherColumns, 2);
        QCOMPARE(starLayout(graph).nodes.size(), 1u);
    }

    void tableWithoutKeysIsAGraphOfOne()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.lonely");
        QCOMPARE(graph.nodes.size(), 1u);
        QVERIFY(graph.edges.empty());
        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 1u);
    }

    void graphFromTheServerLaysOut()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.orders");
        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), graph.nodes.size());
        QVERIFY(!overlap(layout));
        for (const ErdPlacement &placement : layout.nodes)
            QVERIFY(layout.bounds.contains(placement.box));
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

    Oid oidOf(const char *table)
    {
        const QueryOutcome o = run(QByteArray("SELECT '") + table + "'::regclass::oid");
        return o.ok() && !o.results.empty() ? o.results[0].value(0, 0).toUInt() : 0;
    }

    ErdGraph graphOf(const char *table)
    {
        const Oid oid = oidOf(table);
        [&] { QVERIFY(oid > 0); }();
        std::vector<Result> results;
        for (const QByteArray &query : erdQueries(oid)) {
            const QueryOutcome o = run(query);
            [&] {
                QVERIFY2(o.ok(),
                         qPrintable(o.error + QLatin1String(": ") + QString::fromUtf8(query)));
            }();
            results.push_back(o.results.empty() ? Result() : o.results.back());
        }
        return parseErd(oid, results);
    }

    Connection m_connection;
};

QTEST_MAIN(TestErd)
#include "tst_erd.moc"
