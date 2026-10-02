// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Erd.h"
#include "pg/QueryRunner.h"

#include <QElapsedTimer>
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
CREATE TABLE keyless (a int, b text);
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

// Which layer a box ended up on, by its top edge.
std::vector<qreal> rowsOf(const ErdLayout &layout)
{
    std::vector<qreal> tops;
    for (const ErdPlacement &placement : layout.nodes) {
        if (std::ranges::find(tops, placement.box.top()) == tops.end())
            tops.push_back(placement.box.top());
    }
    std::ranges::sort(tops);
    return tops;
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

    // Round the focus table, not in one long row: eight neighbours used to
    // make a diagram four screens wide.
    void starIsRoundNotWide()
    {
        ErdGraph graph;
        graph.focus = 1;
        graph.nodes = {makeNode(1, "orders", 4)};
        for (unsigned int i = 2; i <= 9; ++i) {
            graph.nodes.push_back(
                makeNode(i, QStringLiteral("t%1").arg(i).toUtf8().constData(), 3));
            // Four tables the focus points at, four pointing at it.
            graph.edges.push_back(
                i % 2 == 0 ? makeEdge(QStringLiteral("e%1").arg(i).toUtf8().constData(), 1, i)
                           : makeEdge(QStringLiteral("e%1").arg(i).toUtf8().constData(), i, 1));
        }

        const ErdLayout layout = starLayout(graph);
        QCOMPARE(layout.nodes.size(), 9u);
        QVERIFY(!overlap(layout));

        // Wider than tall is fine; four times as wide is not.
        const qreal ratio = layout.bounds.width() / layout.bounds.height();
        QVERIFY2(
            ratio < 2.5,
            qPrintable(
                QStringLiteral("%1 x %2").arg(layout.bounds.width()).arg(layout.bounds.height())));
        // And the focus table is in the middle of it all.
        const QRectF focus = layout.placement(1)->box;
        QVERIFY(qAbs(focus.center().x() - layout.bounds.center().x()) < focus.width());
        QVERIFY(qAbs(focus.center().y() - layout.bounds.center().y()) < focus.height());
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

    // The layered layout, which is what a whole schema gets.

    void layersPutReferencedTablesAbove()
    {
        // country ← orders ← order_line, a chain of three.
        ErdGraph graph;
        graph.nodes
            = {makeNode(1, "orders", 3), makeNode(2, "country", 2), makeNode(3, "order_line", 2)};
        graph.edges
            = {makeEdge("orders_country_fkey", 1, 2), makeEdge("order_line_orders_fkey", 3, 1)};

        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 3u);
        QCOMPARE(rowsOf(layout).size(), 3u);
        QVERIFY(layout.placement(2)->box.bottom() < layout.placement(1)->box.top());
        QVERIFY(layout.placement(1)->box.bottom() < layout.placement(3)->box.top());
        QVERIFY(!overlap(layout));
    }

    void cyclesDoNotHangTheLayout()
    {
        // Two tables pointing at each other, and a longer ring behind them.
        ErdGraph graph;
        graph.nodes = {makeNode(1, "a", 2), makeNode(2, "b", 2), makeNode(3, "c", 2)};
        graph.edges = {makeEdge("a_b_fkey", 1, 2), makeEdge("b_a_fkey", 2, 1),
                       makeEdge("b_c_fkey", 2, 3), makeEdge("c_a_fkey", 3, 1)};

        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 3u);
        QVERIFY(!overlap(layout));
    }

    void partsThatAreNotConnectedArePackedSideBySide()
    {
        ErdGraph graph;
        graph.nodes = {makeNode(1, "a", 2), makeNode(2, "b", 2), makeNode(3, "x", 2),
                       makeNode(4, "y", 2), makeNode(5, "alone", 1)};
        graph.edges = {makeEdge("a_b_fkey", 1, 2), makeEdge("x_y_fkey", 3, 4)};

        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 5u);
        QVERIFY(!overlap(layout));
        // Each pair keeps its own two layers rather than being spread out.
        QVERIFY(layout.placement(2)->box.bottom() < layout.placement(1)->box.top());
        QVERIFY(layout.placement(4)->box.bottom() < layout.placement(3)->box.top());
        // The parts stand next to each other, not on top of one another.
        QVERIFY(layout.placement(1)->box.right() < layout.placement(3)->box.left()
                || layout.placement(3)->box.right() < layout.placement(1)->box.left());
    }

    // An edge crossing a layer gets a channel of its own, so it is not
    // drawn straight through the tables in between.
    void longEdgesAreRoutedAroundTables()
    {
        // a ← b ← c, and a long one from a straight down to c.
        ErdGraph graph;
        graph.nodes = {makeNode(1, "a", 2), makeNode(2, "b", 2), makeNode(3, "c", 2)};
        graph.edges
            = {makeEdge("b_a_fkey", 2, 1), makeEdge("c_b_fkey", 3, 2), makeEdge("c_a_fkey", 3, 1)};

        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 3u);
        // The short hops need no help.
        QVERIFY(layout.route(2, 1).empty());
        QVERIFY(layout.route(3, 2).empty());

        // The long one bends once, on b's layer and clear of b's box.
        const std::vector<QPointF> bends = layout.route(3, 1);
        QCOMPARE(bends.size(), 1u);
        const QRectF b = layout.placement(2)->box;
        QVERIFY(bends[0].y() > b.top() && bends[0].y() < b.bottom());
        QVERIFY2(!b.contains(bends[0]),
                 qPrintable(QStringLiteral("bend %1,%2 inside %3..%4")
                                .arg(bends[0].x())
                                .arg(bends[0].y())
                                .arg(b.left())
                                .arg(b.right())));
        QVERIFY(!overlap(layout));
    }

    void layeredLayoutIsDeterministic()
    {
        ErdGraph graph;
        for (unsigned int i = 1; i <= 8; ++i)
            graph.nodes.push_back(
                makeNode(i, QStringLiteral("t%1").arg(i).toUtf8().constData(), 2));
        graph.edges = {makeEdge("e1", 2, 1), makeEdge("e2", 3, 1), makeEdge("e3", 4, 2),
                       makeEdge("e4", 5, 2), makeEdge("e5", 6, 3), makeEdge("e6", 7, 4),
                       makeEdge("e7", 8, 5), makeEdge("e8", 8, 6)};

        const ErdLayout first = layeredLayout(graph);
        std::ranges::reverse(graph.nodes);
        std::ranges::reverse(graph.edges);
        const ErdLayout second = layeredLayout(graph);
        for (const ErdPlacement &placement : first.nodes)
            QCOMPARE(second.placement(placement.oid)->box, placement.box);
    }

    void selfReferencesAreNoLayer()
    {
        ErdGraph graph;
        graph.nodes = {makeNode(1, "orders", 3)};
        graph.edges = {makeEdge("orders_parent_fkey", 1, 1)};

        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 1u);
        QCOMPARE(rowsOf(layout).size(), 1u);
    }

    void aSchemaFullOfTablesFallsBackToAGrid()
    {
        ErdGraph graph;
        for (unsigned int i = 1; i <= ManyTables + 20; ++i) {
            graph.nodes.push_back(makeNode(
                i, QStringLiteral("t%1").arg(i, 4, 10, QLatin1Char('0')).toUtf8().constData(), 2));
        }
        QElapsedTimer timer;
        timer.start();
        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), graph.nodes.size());
        QVERIFY(!overlap(layout));
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    }

    void aBigSchemaStillLaysOutQuickly()
    {
        // A hundred tables in a tree: the layered path, at its usual size.
        ErdGraph graph;
        for (unsigned int i = 1; i <= 100; ++i) {
            graph.nodes.push_back(makeNode(
                i, QStringLiteral("t%1").arg(i, 3, 10, QLatin1Char('0')).toUtf8().constData(), 3));
            if (i > 1)
                graph.edges.push_back(
                    makeEdge(QStringLiteral("e%1").arg(i).toUtf8().constData(), i, i / 2));
        }
        QElapsedTimer timer;
        timer.start();
        const ErdLayout layout = layeredLayout(graph);
        QCOMPARE(layout.nodes.size(), 100u);
        QVERIFY(!overlap(layout));
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    }

    void layoutForPicksTheShape()
    {
        ErdGraph focused;
        focused.focus = 1;
        focused.nodes = {makeNode(1, "orders", 2), makeNode(2, "customer", 2)};
        focused.edges = {makeEdge("orders_customer_fkey", 1, 2)};
        QCOMPARE(layoutFor(focused).nodes.size(), starLayout(focused).nodes.size());

        ErdGraph schema = focused;
        schema.focus = 0; // A whole schema singles no table out.
        QCOMPARE(layoutFor(schema).placement(1)->box, layeredLayout(schema).placement(1)->box);
    }

    // Several schemas: each one framed and laid out on its own.

    void clustersKeepSchemasApart()
    {
        ErdGraph graph;
        auto add = [&graph](unsigned int oid, const char *schema, const char *name) {
            ErdNode node = makeNode(oid, name, 2);
            node.schema = QString::fromUtf8(schema);
            graph.nodes.push_back(node);
        };
        add(1, "sales", "orders");
        add(2, "sales", "customer");
        add(3, "stock", "item");
        add(4, "stock", "warehouse");
        graph.edges = {makeEdge("orders_customer_fkey", 1, 2), makeEdge("item_wh_fkey", 3, 4),
                       makeEdge("orders_item_fkey", 1, 3)}; // The last one crosses schemas.

        const ErdLayout layout = clusteredLayout(graph);
        QCOMPARE(layout.nodes.size(), 4u);
        QVERIFY(!overlap(layout));

        QCOMPARE(layout.clusters.size(), 2u);
        QCOMPARE(layout.clusters[0].name, QStringLiteral("sales")); // In name order.
        QCOMPARE(layout.clusters[1].name, QStringLiteral("stock"));
        QVERIFY(!layout.clusters[0].box.intersects(layout.clusters[1].box));

        // Every table sits inside its own schema's frame.
        QVERIFY(layout.clusters[0].box.contains(layout.placement(1)->box));
        QVERIFY(layout.clusters[0].box.contains(layout.placement(2)->box));
        QVERIFY(layout.clusters[1].box.contains(layout.placement(3)->box));
        QVERIFY(layout.clusters[1].box.contains(layout.placement(4)->box));
        QVERIFY(layout.bounds.contains(layout.clusters[1].box));

        // Inside a frame the keys still decide what goes above what.
        QVERIFY(layout.placement(2)->box.bottom() < layout.placement(1)->box.top());
        QVERIFY(layout.placement(4)->box.bottom() < layout.placement(3)->box.top());
    }

    void clusteredLayoutIsDeterministic()
    {
        ErdGraph graph;
        for (unsigned int i = 1; i <= 9; ++i) {
            ErdNode node = makeNode(i, QStringLiteral("t%1").arg(i).toUtf8().constData(), 2);
            node.schema = QStringLiteral("s%1").arg(i % 3);
            graph.nodes.push_back(node);
        }
        graph.edges = {makeEdge("e1", 4, 1), makeEdge("e2", 7, 4), makeEdge("e3", 5, 2)};

        const ErdLayout first = clusteredLayout(graph);
        std::ranges::reverse(graph.nodes);
        std::ranges::reverse(graph.edges);
        const ErdLayout second = clusteredLayout(graph);
        QCOMPARE(first.clusters.size(), 3u);
        for (const ErdPlacement &placement : first.nodes)
            QCOMPARE(second.placement(placement.oid)->box, placement.box);
        for (std::size_t i = 0; i < first.clusters.size(); ++i) {
            QCOMPARE(second.clusters[i].name, first.clusters[i].name);
            QCOMPARE(second.clusters[i].box, first.clusters[i].box);
        }
    }

    void layoutForFramesSeveralSchemas()
    {
        ErdGraph graph;
        ErdNode here = makeNode(1, "a", 2);
        ErdNode there = makeNode(2, "b", 2);
        there.schema = QStringLiteral("other");
        graph.nodes = {here, there};
        QCOMPARE(layoutFor(graph).clusters.size(), 2u);

        // One schema needs no frames at all.
        graph.nodes[1].schema = graph.nodes[0].schema;
        QVERIFY(layoutFor(graph).clusters.empty());
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

    // What the crow's foot needs: a key that cannot be null means its table
    // must have exactly one counterpart, not zero or one.
    void keysSayWhetherTheyAreMandatory()
    {
        const ErdGraph graph = graphOf("slonisko_erd_test.orders");
        const Oid orders = oidOf("slonisko_erd_test.orders");
        const auto edge = [&](const char *table) {
            const Oid other = oidOf((QByteArray("slonisko_erd_test.") + table).constData());
            return std::ranges::find_if(
                graph.edges, [&](const ErdEdge &e) { return e.from == orders && e.to == other; });
        };
        QVERIFY(edge("customer")->mandatory); // customer_id is NOT NULL.
        QVERIFY(!edge("country")->mandatory); // country_code may be null.
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

    // A whole schema: every table in it, and the ones its keys point at.
    void schemaGraph()
    {
        const QueryOutcome oid = run("SELECT oid FROM pg_namespace "
                                     "WHERE nspname = 'slonisko_erd_test'");
        QVERIFY(oid.ok());
        const ErdGraph graph = graphOfSchema(oid.results[0].value(0, 0).toUInt());

        QStringList names;
        for (const ErdNode &node : graph.nodes)
            names << node.name;
        std::ranges::sort(names);
        QCOMPARE(names,
                 (QStringList {QStringLiteral("country"), QStringLiteral("customer"),
                               QStringLiteral("keyless"), QStringLiteral("lonely"),
                               QStringLiteral("order_line"), QStringLiteral("orders"),
                               QStringLiteral("shipment")}));
        QCOMPARE(graph.focus, 0u); // No table is singled out.
        // Every foreign key of the schema is there, the self-reference too.
        QCOMPARE(graph.edges.size(), 5u);

        const ErdLayout layout = layoutFor(graph);
        QCOMPARE(layout.nodes.size(), graph.nodes.size());
        QVERIFY(!overlap(layout));
        // order_line points at orders, so it hangs below it.
        const ErdNode *orders = nodeNamed(graph, "orders");
        const ErdNode *lines = nodeNamed(graph, "order_line");
        QVERIFY(orders && lines);
        QVERIFY(layout.placement(orders->oid)->box.bottom()
                < layout.placement(lines->oid)->box.top());
    }

    // The whole database: every schema but the server's own.
    void databaseGraph()
    {
        std::vector<Result> results;
        for (const QByteArray &query : erdQueries(ErdScope::Database, 0)) {
            const QueryOutcome o = run(query);
            QVERIFY2(o.ok(), qPrintable(o.error + QLatin1String(": ") + QString::fromUtf8(query)));
            results.push_back(o.results.empty() ? Result() : o.results.back());
        }
        const ErdGraph graph = parseErd(0, results);

        QStringList schemas;
        for (const ErdNode &node : graph.nodes) {
            if (!schemas.contains(node.schema))
                schemas << node.schema;
        }
        QVERIFY(schemas.contains(QStringLiteral("slonisko_erd_test")));
        QVERIFY(!schemas.contains(QStringLiteral("pg_catalog")));
        QVERIFY(!schemas.contains(QStringLiteral("information_schema")));
        QVERIFY(nodeNamed(graph, "orders"));

        const ErdLayout layout = layoutFor(graph);
        QCOMPARE(layout.nodes.size(), graph.nodes.size());
        QVERIFY(!overlap(layout));
    }

    // A table with no keys at all must still turn up, or its diagram would
    // be empty rather than a box on its own.
    void tableWithNoKeyColumns()
    {
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

    static const ErdNode *nodeNamed(const ErdGraph &graph, const char *name)
    {
        const auto it = std::ranges::find(graph.nodes, QString::fromUtf8(name), &ErdNode::name);
        return it == graph.nodes.end() ? nullptr : &*it;
    }

    ErdGraph graphOfSchema(Oid schema)
    {
        std::vector<Result> results;
        for (const QByteArray &query : erdQueries(ErdScope::Schema, schema)) {
            const QueryOutcome o = run(query);
            [&] {
                QVERIFY2(o.ok(),
                         qPrintable(o.error + QLatin1String(": ") + QString::fromUtf8(query)));
            }();
            results.push_back(o.results.empty() ? Result() : o.results.back());
        }
        return parseErd(0, results);
    }

    ErdGraph graphOf(const char *table)
    {
        const Oid oid = oidOf(table);
        [&] { QVERIFY(oid > 0); }();
        std::vector<Result> results;
        for (const QByteArray &query : erdQueries(ErdScope::Table, oid)) {
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
