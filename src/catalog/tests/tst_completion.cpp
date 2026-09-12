// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Completion.h"

#include <QTest>

using namespace slonisko::catalog;
using Context = Completion::Context;
using Kind = CompletionItem::Kind;

namespace {

Snapshot makeSnapshot()
{
    auto columns = [](std::initializer_list<const char *> names) {
        std::vector<Column> out;
        for (const char *n : names)
            out.push_back({QString::fromUtf8(n), QStringLiteral("integer")});
        return out;
    };
    Snapshot s;
    s.searchPath = {QStringLiteral("pg_catalog"), QStringLiteral("public")};
    s.schemas = {QStringLiteral("information_schema"), QStringLiteral("pg_catalog"),
                 QStringLiteral("public"), QStringLiteral("sales")};
    s.relations = {
        {1, QStringLiteral("public"), QStringLiteral("orders"), 'r',
         columns({"id", "customer_id", "total", "created_at"})},
        {2, QStringLiteral("public"), QStringLiteral("customers"), 'r',
         columns({"id", "name", "customer_name"})},
        {3, QStringLiteral("public"), QStringLiteral("recent_orders"), 'v', columns({"id"})},
        {4, QStringLiteral("public"), QStringLiteral("OrderItems"), 'r', columns({"Qty"})},
        {5, QStringLiteral("sales"), QStringLiteral("invoices"), 'r', columns({"id", "amount"})},
        {6, QStringLiteral("pg_catalog"), QStringLiteral("pg_class"), 'r',
         columns({"oid", "relname"})},
    };
    s.functions = {
        {QStringLiteral("pg_catalog"), QStringLiteral("count"), QStringLiteral("\"any\""),
         QStringLiteral("bigint"), 'a'},
        {QStringLiteral("public"), QStringLiteral("order_total"), QStringLiteral("integer"),
         QStringLiteral("numeric"), 'f'},
        {QStringLiteral("sales"), QStringLiteral("tax"), QStringLiteral("numeric"),
         QStringLiteral("numeric"), 'f'},
    };
    s.types = {{QStringLiteral("pg_catalog"), QStringLiteral("integer")},
               {QStringLiteral("pg_catalog"), QStringLiteral("text")},
               {QStringLiteral("public"), QStringLiteral("mood")}};
    return s;
}

// The statement with | at the cursor.
Completion completeAt(const char *withCursor)
{
    static const Snapshot snapshot = makeSnapshot();
    QByteArray sql(withCursor);
    const qsizetype cursor = sql.indexOf('|');
    sql.remove(cursor, 1);
    return complete(sql, cursor, snapshot);
}

QStringList labels(const Completion &c, std::optional<Kind> kind = std::nullopt)
{
    QStringList out;
    for (const CompletionItem &item : c.items) {
        if (!kind || item.kind == *kind)
            out << item.label;
    }
    return out;
}

const CompletionItem *item(const Completion &c, const QString &label)
{
    for (const CompletionItem &i : c.items) {
        if (i.label == label)
            return &i;
    }
    return nullptr;
}

} // namespace

class TestCompletion : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void relationsAfterFrom()
    {
        const Completion c = completeAt("SELECT * FROM |");
        QCOMPARE(c.context, Context::Relation);
        const QStringList tables = labels(c, Kind::Table);
        QVERIFY(tables.contains(QStringLiteral("orders")));
        QVERIFY(tables.contains(QStringLiteral("customers")));
        QVERIFY(!tables.contains(QStringLiteral("invoices"))); // Not in search_path.
        QVERIFY(labels(c, Kind::View).contains(QStringLiteral("recent_orders")));
        QVERIFY(labels(c, Kind::Schema).contains(QStringLiteral("sales")));
        // Needs quotes.
        QCOMPARE(item(c, QStringLiteral("OrderItems"))->insertText,
                 QStringLiteral("\"OrderItems\""));
        // User objects before system ones.
        QVERIFY(labels(c).indexOf(QStringLiteral("orders"))
                < labels(c).indexOf(QStringLiteral("pg_class")));
    }

    void relationsInSchema()
    {
        const Completion c = completeAt("SELECT * FROM sales.|");
        QCOMPARE(c.context, Context::Relation);
        QCOMPARE(labels(c, Kind::Table), QStringList {QStringLiteral("invoices")});
    }

    void prefixReplacesTheWord()
    {
        const Completion c = completeAt("SELECT * FROM ord|ers2");
        QCOMPARE(c.prefix, QStringLiteral("ord"));
        QCOMPARE(c.replaceFrom, 14);
        QCOMPARE(c.replaceTo, 21);
        QCOMPARE(c.items.front().label, QStringLiteral("orders"));
    }

    void columnsInScope()
    {
        const Completion c = completeAt("SELECT | FROM orders");
        QCOMPARE(c.context, Context::Column);
        const QStringList columns = labels(c, Kind::Column);
        QVERIFY(columns.contains(QStringLiteral("customer_id")));
        QVERIFY(!columns.contains(QStringLiteral("customer_name"))); // customers is not in scope.
        QCOMPARE(c.items.front().kind, Kind::Column);
        QVERIFY(item(c, QStringLiteral("total"))->detail.contains(QLatin1String("integer")));
    }

    void aliasBeforeFromIsTyped()
    {
        // The select list is typed before FROM exists... or before it is complete.
        const Completion c
            = completeAt("SELECT o.| FROM orders o JOIN customers c ON c.id = o.customer_id");
        QCOMPARE(c.context, Context::Qualified);
        QCOMPARE(labels(c, Kind::Column),
                 (QStringList {QStringLiteral("created_at"), QStringLiteral("customer_id"),
                               QStringLiteral("id"), QStringLiteral("total")}));
    }

    void aliasInBrokenStatement()
    {
        const Completion c = completeAt("SELECT c.| FROM orders o JOIN customers c ON");
        QCOMPARE(c.context, Context::Qualified);
        QVERIFY(labels(c).contains(QStringLiteral("customer_name")));
        QVERIFY(!labels(c).contains(QStringLiteral("total")));
    }

    void tableNameAsQualifier()
    {
        const Completion c = completeAt("SELECT customers.| FROM customers");
        QVERIFY(labels(c).contains(QStringLiteral("name")));
        const Completion schema = completeAt("SELECT * FROM orders WHERE sales.invoices.|");
        QVERIFY(labels(schema).contains(QStringLiteral("amount")));
    }

    void whereClause()
    {
        const Completion c = completeAt("SELECT * FROM orders WHERE cust|");
        QCOMPARE(c.context, Context::Column);
        QCOMPARE(c.items.front().label, QStringLiteral("customer_id"));
    }

    void fuzzyMatch()
    {
        const Completion c = completeAt("SELECT cnm| FROM customers");
        QCOMPARE(c.items.front().label, QStringLiteral("customer_name"));
    }

    void keywordsAfterName()
    {
        Completion c = completeAt("SELECT * FROM orders WH|");
        QCOMPARE(c.context, Context::Keyword);
        QCOMPARE(c.items.front().label, QStringLiteral("WHERE"));

        c = completeAt("select * from orders o wh|");
        QCOMPARE(c.items.front().label, QStringLiteral("where"));

        c = completeAt("SELECT id FR|");
        QCOMPARE(c.context, Context::Keyword);
        QCOMPARE(c.items.front().label, QStringLiteral("FROM"));

        c = completeAt("SELECT * FROM orders WHERE id = 1 AN|");
        QCOMPARE(c.items.front().label, QStringLiteral("AND"));
    }

    void insertColumns()
    {
        Completion c = completeAt("INSERT INTO orders (|");
        QCOMPARE(c.context, Context::Column);
        QVERIFY(labels(c, Kind::Column).contains(QStringLiteral("total")));
        QVERIFY(labels(c, Kind::Function).isEmpty());

        c = completeAt("INSERT INTO orders (id, |) VALUES (1, 2)");
        QCOMPARE(c.context, Context::Column);
        QVERIFY(labels(c, Kind::Column).contains(QStringLiteral("customer_id")));
    }

    void updateSet()
    {
        const Completion c = completeAt("UPDATE orders SET | = 1 WHERE id = 2");
        QCOMPARE(c.context, Context::Column);
        QVERIFY(labels(c, Kind::Column).contains(QStringLiteral("total")));
    }

    void cteColumns()
    {
        const Completion c = completeAt(
            "WITH recent AS (SELECT id, total AS amount FROM orders) SELECT r.| FROM recent r");
        QCOMPARE(labels(c, Kind::Column),
                 (QStringList {QStringLiteral("amount"), QStringLiteral("id")}));

        const Completion from = completeAt("WITH recent AS (SELECT 1) SELECT * FROM rec|");
        QCOMPARE(from.items.front().label, QStringLiteral("recent"));
        QCOMPARE(from.items.front().kind, Kind::Cte);
    }

    void subqueryColumns()
    {
        const Completion c = completeAt("SELECT s.| FROM (SELECT id AS sid, total FROM orders) s");
        QCOMPARE(labels(c, Kind::Column),
                 (QStringList {QStringLiteral("sid"), QStringLiteral("total")}));
    }

    void severalSources()
    {
        const Completion c = completeAt("SELECT * FROM orders o, customers c WHERE |");
        const QStringList columns = labels(c, Kind::Column);
        QVERIFY(columns.contains(QStringLiteral("total")));
        QVERIFY(columns.contains(QStringLiteral("customer_name")));
        QVERIFY(labels(c, Kind::Alias).contains(QStringLiteral("o")));
    }

    void types()
    {
        const Completion c = completeAt("CREATE TABLE t (a |)");
        QCOMPARE(c.context, Context::Type);
        QVERIFY(labels(c, Kind::Type).contains(QStringLiteral("integer")));
        QVERIFY(labels(c, Kind::Type).contains(QStringLiteral("mood")));

        const Completion cast = completeAt("SELECT 1::te|");
        QCOMPARE(cast.context, Context::Type);
        QCOMPARE(cast.items.front().label, QStringLiteral("text"));
    }

    void functions()
    {
        Completion c = completeAt("SELECT coun| FROM orders");
        QCOMPARE(c.items.front().label, QStringLiteral("count"));
        QCOMPARE(c.items.front().kind, Kind::Function);

        c = completeAt("SELECT count(|) FROM orders");
        QCOMPARE(c.context, Context::Column);
        QVERIFY(labels(c, Kind::Column).contains(QStringLiteral("total")));

        c = completeAt("SELECT sales.t|(1)");
        QCOMPARE(c.context, Context::Function);
        QCOMPARE(labels(c, Kind::Function), QStringList {QStringLiteral("tax")});
    }

    void nothingInLiterals()
    {
        QCOMPARE(completeAt("SELECT 'abc|'").context, Context::None);
        QCOMPARE(completeAt("SELECT 1 -- comm|").context, Context::None);
        QCOMPARE(completeAt("SELECT /* x| */ 1").context, Context::None);
        QCOMPARE(completeAt("SELECT $$ab|$$").context, Context::None);
        // Right after a literal is not in it.
        QCOMPARE(completeAt("SELECT 'abc' |").context, Context::Keyword);
    }

    void functionBody()
    {
        const QByteArray sql
            = "CREATE FUNCTION f() RETURNS int AS $$ SELECT to| FROM orders $$ LANGUAGE sql";
        const Completion c = completeAt(sql.constData());
        QCOMPARE(c.context, Context::Column);
        QCOMPARE(c.items.front().label, QStringLiteral("total"));
        QCOMPARE(c.replaceFrom, sql.indexOf("to|"));
    }

    void quotedPrefix()
    {
        const Completion c = completeAt("SELECT * FROM \"Ord|");
        QCOMPARE(c.prefix, QStringLiteral("Ord"));
        QCOMPARE(c.replaceFrom, 14);
        QCOMPARE(c.items.front().insertText, QStringLiteral("\"OrderItems\""));
    }

    void emptyStatement()
    {
        const Completion c = completeAt("|");
        QCOMPARE(c.context, Context::Keyword);
        QVERIFY(labels(c).contains(QStringLiteral("SELECT")));
    }

    void scores()
    {
        QVERIFY(fuzzyScore(QStringLiteral("ord"), QStringLiteral("orders"))
                > fuzzyScore(QStringLiteral("ord"), QStringLiteral("recent_orders")));
        QVERIFY(fuzzyScore(QStringLiteral("cnm"), QStringLiteral("customer_name"))
                > fuzzyScore(QStringLiteral("cnm"), QStringLiteral("cinema")));
        QVERIFY(fuzzyScore(QStringLiteral("oi"), QStringLiteral("OrderItems")) > 500);
        // Letters out of one word and into the next, "leapa" for
        // "learning_package", and the better lined-up name wins.
        QVERIFY(fuzzyScore(QStringLiteral("leapa"), QStringLiteral("learning_package")) > 300);
        QVERIFY(fuzzyScore(QStringLiteral("lepac"), QStringLiteral("learning_package"))
                > fuzzyScore(QStringLiteral("lepac"), QStringLiteral("lesson_plan_archive")));
        QVERIFY(fuzzyScore(QStringLiteral("lpkg"), QStringLiteral("learning_package")) > 0);
        // A run of letters beats the same letters scattered about.
        QVERIFY(fuzzyScore(QStringLiteral("lpack"), QStringLiteral("learning_package"))
                > fuzzyScore(QStringLiteral("lpcka"), QStringLiteral("learning_package")));
        QCOMPARE(fuzzyScore(QStringLiteral("zzz"), QStringLiteral("learning_package")), 0);
    }

    void matchPositions()
    {
        using Positions = std::vector<qsizetype>;
        // What to highlight in the popup: "lea" and then "pa" of "package".
        QCOMPARE(fuzzyMatchPositions(QStringLiteral("leapa"), QStringLiteral("learning_package")),
                 (Positions {0, 1, 2, 9, 10}));
        QCOMPARE(fuzzyMatchPositions(QStringLiteral("lea"), QStringLiteral("learning_package")),
                 (Positions {0, 1, 2}));
        QCOMPARE(fuzzyMatchPositions(QStringLiteral("pack"), QStringLiteral("learning_package")),
                 (Positions {9, 10, 11, 12}));
        QCOMPARE(fuzzyMatchPositions(QStringLiteral("oi"), QStringLiteral("OrderItems")),
                 (Positions {0, 5}));
        QVERIFY(fuzzyMatchPositions(QStringLiteral("zzz"), QStringLiteral("orders")).empty());
        QCOMPARE(fuzzyScore(QStringLiteral("xyz"), QStringLiteral("orders")), 0);
        QVERIFY(fuzzyScore(QString(), QStringLiteral("orders")) > 0);
    }

    void textArrays()
    {
        QCOMPARE(parseTextArray(QStringLiteral("{pg_catalog,public}")),
                 (QStringList {QStringLiteral("pg_catalog"), QStringLiteral("public")}));
        QCOMPARE(parseTextArray(QStringLiteral("{\"my schema\",\"a\\\"b\"}")),
                 (QStringList {QStringLiteral("my schema"), QStringLiteral("a\"b")}));
        QCOMPARE(parseTextArray(QStringLiteral("{}")), QStringList {});
    }
};

QTEST_GUILESS_MAIN(TestCompletion)
#include "tst_completion.moc"
