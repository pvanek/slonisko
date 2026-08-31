// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Plan.h"
#include "pg/QueryRunner.h"

#include <QFile>
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
