// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Plan.h"
#include "catalog/PlanExport.h"
#include "catalog/PlanGraph.h"
#include "pg/QueryRunner.h"

#include <QFile>
#include <QRegularExpression>
#include <QTest>

using namespace slonisko::catalog;
using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;
using slonisko::pg::QueryRunner;

namespace {

const QByteArray Estimated = R"json([
  {
    "Plan": {
      "Node Type": "Hash Join", "Parallel Aware": false, "Join Type": "Left",
      "Startup Cost": 10.0, "Total Cost": 100.0, "Plan Rows": 50, "Plan Width": 8,
      "Hash Cond": "(o.customer_id = c.id)",
      "Plans": [
        {"Node Type": "Seq Scan", "Parent Relationship": "Outer", "Relation Name": "orders",
         "Schema": "public", "Alias": "o", "Startup Cost": 0.0, "Total Cost": 60.0,
         "Plan Rows": 50, "Plan Width": 8, "Filter": "(total > 10)"},
        {"Node Type": "Hash", "Parent Relationship": "Inner", "Startup Cost": 10.0,
         "Total Cost": 10.0, "Plan Rows": 10, "Plan Width": 4,
         "Plans": [
           {"Node Type": "Index Only Scan", "Parent Relationship": "Outer", "Scan Direction": "Forward",
            "Index Name": "customers_pkey", "Relation Name": "customers", "Schema": "public",
            "Alias": "c", "Startup Cost": 0.0, "Total Cost": 10.0, "Plan Rows": 10, "Plan Width": 4}
         ]}
      ]
    }
  }
])json";

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

} // namespace

class TestPlan : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void estimated()
    {
        QString error;
        const auto plan = parsePlan(Estimated, &error);
        QVERIFY2(plan, qPrintable(error));
        QVERIFY(!plan->analyzed);
        QVERIFY(!plan->executionMs);

        const PlanNode &join = plan->root;
        QCOMPARE(join.nodeType, QStringLiteral("Hash Left Join"));
        QCOMPARE(join.details, QStringList {QStringLiteral("Hash Cond: (o.customer_id = c.id)")});
        QCOMPARE(join.children.size(), 2u);

        const PlanNode &scan = join.children[0];
        QCOMPARE(scan.nodeType, QStringLiteral("Seq Scan"));
        QCOMPARE(scan.object, QStringLiteral("public.orders o"));
        QCOMPARE(scan.details, QStringList {QStringLiteral("Filter: (total > 10)")});

        const PlanNode &index = join.children[1].children[0];
        QCOMPARE(index.object, QStringLiteral("public.customers c using customers_pkey"));

        // Cost of its own: 100 - 60 - 10.
        QCOMPARE(join.exclusive, 30.0);
        QCOMPARE(join.exclusiveShare, 0.3);
        QCOMPARE(scan.exclusiveShare, 0.6);
        QCOMPARE(join.children[1].exclusive, 0.0); // The Hash costs nothing beyond its child.
    }

    void asText()
    {
        const auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        // psql's shape: children behind "->", details indented under their
        // node, costs after the node's name.
        const QStringList lines = planText(*plan).split(QLatin1Char('\n'));
        QVERIFY(lines.first().startsWith(QLatin1String("Hash Left Join  (cost=")));
        QCOMPARE(lines.value(1), QStringLiteral("  Hash Cond: (o.customer_id = c.id)"));
        QVERIFY(
            lines.value(2).startsWith(QLatin1String("  ->  Seq Scan on public.orders o  (cost=")));
        QCOMPARE(lines.value(3), QStringLiteral("        Filter: (total > 10)"));
        QVERIFY(lines.value(4).startsWith(QLatin1String("  ->  Hash  (cost=")));
        QVERIFY(lines.value(5).startsWith(QLatin1String(
            "        ->  Index Only Scan on public.customers c using customers_pkey  (cost=")));
        QVERIFY(!planText(*plan).contains(QLatin1String("actual time"))); // Not analyzed.
        QVERIFY(planText(Plan {}).isEmpty());
    }

    // Leaves side by side, each parent over the middle of its children.
    void diagramLayout()
    {
        const auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        const PlanDiagramLayout layout = layoutPlanDiagram(*plan, QSizeF(100, 50), QSizeF(20, 30));
        QCOMPARE(layout.boxes.size(), std::size_t(4));
        const auto &[join, scan, hash, index]
            = std::tie(layout.boxes[0], layout.boxes[1], layout.boxes[2], layout.boxes[3]);
        QCOMPARE(join.node, &plan->root);
        QCOMPARE(join.parent, -1);
        QCOMPARE(scan.rect, QRectF(0, 80, 100, 50));
        QCOMPARE(hash.rect, QRectF(120, 80, 100, 50));
        QCOMPARE(index.rect, QRectF(120, 160, 100, 50));
        QCOMPARE(index.parent, 2);
        QCOMPARE(index.depth, 2);
        QCOMPARE(join.rect, QRectF(60, 0, 100, 50));
        QCOMPARE(layout.size, QSizeF(220, 210));
        QCOMPARE(layout.maxRows, 50.0);
        QVERIFY(layoutPlanDiagram(Plan {}, QSizeF(100, 50), QSizeF(20, 30)).boxes.empty());

        QCOMPARE(edgeWidth(0, 1000, 8), 1.0);
        QCOMPARE(edgeWidth(1000, 1000, 8), 8.0);
        QVERIFY(edgeWidth(10, 1000, 8) < edgeWidth(100, 1000, 8));
    }

    // Each frame as wide as its share of the root, its children above it;
    // what is left over beside them is the parent's own.
    void flame()
    {
        const auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        const std::vector<FlameFrame> frames = flameGraph(*plan);
        QCOMPARE(frames.size(), std::size_t(4));
        QCOMPARE(flameDepth(frames), 3);
        QCOMPARE(frames[0].start, 0.0);
        QCOMPARE(frames[0].width, 1.0);
        QCOMPARE(frames[1].start, 0.0); // Seq Scan: 60 of 100.
        QCOMPARE(frames[1].width, 0.6);
        QCOMPARE(frames[2].start, 0.6); // Hash: 10.
        QCOMPARE(frames[2].width, 0.1);
        QCOMPARE(frames[3].depth, 2);
        QCOMPARE(frames[3].start, 0.6);
        QCOMPARE(frames[3].width, 0.1);
        QCOMPARE(frames[3].parent, 2);

        // Children that add up to more than their parent, as parallel
        // workers do, share its width.
        Plan parallel;
        parallel.root.nodeType = QStringLiteral("Gather");
        parallel.root.totalCost = 10;
        for (int i = 0; i < 2; ++i) {
            PlanNode worker;
            worker.nodeType = QStringLiteral("Parallel Seq Scan");
            worker.totalCost = 8;
            parallel.root.children.push_back(worker);
        }
        const std::vector<FlameFrame> scaled = flameGraph(parallel);
        QCOMPARE(scaled[1].width, 0.5);
        QCOMPARE(scaled[2].start, 0.5);
        QCOMPARE(scaled[2].width, 0.5);
        QVERIFY(flameGraph(Plan {}).empty());
    }

    // A line per node that took anything itself, its stack from the root.
    void foldedStacks()
    {
        const auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        QCOMPARE(planToFoldedStacks(*plan),
                 QStringLiteral("Hash Left Join 3000\n"
                                "Hash Left Join;Seq Scan on public.orders o 6000\n"
                                "Hash Left Join;Hash;Index Only Scan on public.customers c "
                                "using customers_pkey 1000\n"));
        QVERIFY(planToFoldedStacks(Plan {}).isEmpty());

        // A semicolon would split a frame in two.
        Plan odd;
        odd.root.nodeType = QStringLiteral("Function Scan");
        odd.root.object = QStringLiteral("f;g");
        odd.root.totalCost = 1;
        odd.root.exclusive = 1;
        QCOMPARE(planToFoldedStacks(odd), QStringLiteral("Function Scan on f,g 100\n"));
    }

    void graphviz()
    {
        auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        plan->root.children[0].object = QStringLiteral("public.\"odd\\name\"");
        const QString dot = planToDot(*plan);
        QVERIFY(dot.startsWith(QLatin1String("digraph plan {\n    rankdir=BT;")));
        QCOMPARE(dot.count(QLatin1String("[label=")), 4);
        QVERIFY(dot.contains(QLatin1String("n1 -> n0 [penwidth=")));
        QVERIFY(dot.contains(QLatin1String("n3 -> n2 [penwidth=")));
        QVERIFY2(dot.contains(QLatin1String("Seq Scan\\npublic.\\\"odd\\\\name\\\"\\n")),
                 qPrintable(dot));
        // The join has no object: its condition instead.
        QVERIFY(dot.contains(QLatin1String("Hash Left Join\\nHash Cond: (o.customer_id = c.id)")));
        QVERIFY(dot.trimmed().endsWith(QLatin1Char('}')));
        QVERIFY(planToDot(Plan {}).isEmpty());
    }

    void mermaid()
    {
        auto plan = parsePlan(Estimated);
        QVERIFY(plan);
        plan->root.children[0].details = {QStringLiteral("Filter: (\"#\" > 10)")};
        plan->root.children[0].object.clear();
        const QString mmd = planToMermaid(*plan);
        QVERIFY(mmd.startsWith(QLatin1String("flowchart BT\n")));
        QVERIFY(mmd.contains(QLatin1String("n1 -->|50 rows| n0")));
        QVERIFY2(mmd.contains(QLatin1String("Filter: (#quot;#35;#quot; #gt; 10)")),
                 qPrintable(mmd));
        // The scan takes 60 % of the plan itself, the join 30 %.
        QVERIFY(mmd.contains(QRegularExpression(QStringLiteral("n1\\[.*\\]:::hot"))));
        QVERIFY(mmd.contains(QRegularExpression(QStringLiteral("n0\\[.*\\]:::hot"))));
        QVERIFY(!mmd.contains(QRegularExpression(QStringLiteral("n2\\[.*\\]:::"))));
        QVERIFY(mmd.contains(QLatin1String("classDef hot")));
    }

    void invalid()
    {
        QString error;
        QVERIFY(!parsePlan("not json", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!parsePlan("[{\"x\": 1}]", &error));
    }

    void fromServer()
    {
        const QByteArray conninfo = serverConninfo();
        if (conninfo.isEmpty())
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        Connection c;
        c.open(conninfo);
        QueryRunner runner(&c);
        std::optional<QueryOutcome> outcome;
        runner.run(
            "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) "
            "SELECT g, count(*) FROM generate_series(1, 1000) g "
            "JOIN generate_series(1, 100) h ON h = g % 100 GROUP BY g ORDER BY 2 DESC LIMIT 5",
            this, [&](const QueryOutcome &o) { outcome = o; });
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 10'000);
        QVERIFY2(outcome->ok(), qPrintable(outcome->error));

        QString error;
        const auto plan = parsePlan(outcome->results.back().value(0, 0), &error);
        QVERIFY2(plan, qPrintable(error));
        QVERIFY(plan->analyzed);
        QVERIFY(plan->executionMs.has_value());
        QVERIFY(plan->planningMs.has_value());
        QCOMPARE(plan->root.nodeType, QStringLiteral("Limit"));
        QVERIFY(plan->root.actualRows.has_value());
        QCOMPARE(*plan->root.actualRows, 5.0);

        // Shares of all nodes add up to the whole.
        double total = 0;
        std::function<void(const PlanNode &)> sum = [&](const PlanNode &n) {
            total += n.exclusiveShare;
            for (const PlanNode &child : n.children)
                sum(child);
        };
        sum(plan->root);
        QVERIFY2(qAbs(total - 1.0) < 0.05, qPrintable(QString::number(total)));
    }
};

QTEST_GUILESS_MAIN(TestPlan)
#include "tst_plan.moc"
