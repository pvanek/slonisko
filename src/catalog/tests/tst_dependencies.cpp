// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Dependencies.h"

#include <QTest>

using namespace slonisko::catalog;

namespace {

bool has(const std::vector<QualifiedName> &names, const QString &schema, const QString &name)
{
    return std::ranges::find(names, QualifiedName {schema, name}) != names.end();
}

} // namespace

// What a function body names, which pg_depend does not know: only the body
// tells that dropping a table breaks the function.
class TestDependencies : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sqlBody()
    {
        const BodyReferences refs = bodyReferences(
            QStringLiteral("sql"),
            "WITH recent AS (SELECT * FROM shop.orders WHERE placed > now() - '1 day'::interval) "
            "INSERT INTO audit (what) SELECT format_order(r.id)::summary FROM recent r");
        QVERIFY(refs.parsed);
        QVERIFY(has(refs.relations, QStringLiteral("shop"), QStringLiteral("orders")));
        QVERIFY(has(refs.relations, QString(), QStringLiteral("audit")));
        // A CTE is the statement's own, not a table.
        QVERIFY(!has(refs.relations, QString(), QStringLiteral("recent")));
        QVERIFY(has(refs.functions, QString(), QStringLiteral("format_order")));
        QVERIFY(has(refs.types, QString(), QStringLiteral("summary")));
        // Built-in types are spelled pg_catalog.interval, and are nobody's concern.
        QVERIFY(!has(refs.types, QStringLiteral("pg_catalog"), QStringLiteral("interval")));
        QVERIFY(refs.columns.contains(QStringLiteral("placed")));
        QVERIFY(refs.columns.contains(QStringLiteral("what")));
        QVERIFY(refs.star);
    }

    void sequenceNamedInAString()
    {
        const BodyReferences refs
            = bodyReferences(QStringLiteral("sql"),
                             "SELECT nextval('Shop.\"Order_Seq\"'), currval('counter'::regclass)");
        QVERIFY(refs.parsed);
        QVERIFY(has(refs.relations, QStringLiteral("shop"), QStringLiteral("Order_Seq")));
        QVERIFY(has(refs.relations, QString(), QStringLiteral("counter")));
    }

    void plpgsqlBody()
    {
        const BodyReferences refs = bodyReferences(QStringLiteral("plpgsql"), {}, R"sql(
CREATE OR REPLACE FUNCTION shop.close(p_id integer)
 RETURNS integer
 LANGUAGE plpgsql
AS $function$
DECLARE
    o shop.orders%ROWTYPE;
    n lines.qty%TYPE;
    r record;
BEGIN
    SELECT * INTO o FROM shop.orders WHERE id = p_id;
    n := (SELECT sum(qty) FROM lines WHERE order_id = p_id);
    FOR r IN SELECT id FROM refunds LOOP
        PERFORM notify_refund(r.id);
    END LOOP;
    IF NOT shop.is_open(o.id) THEN
        RAISE EXCEPTION 'closed';
    END IF;
    UPDATE invoices SET closed = true WHERE order_id = p_id;
    EXECUTE 'DELETE FROM drafts';
    RETURN n;
END
$function$)sql");
        QVERIFY2(refs.parsed, qPrintable(refs.error));
        QVERIFY(has(refs.relations, QStringLiteral("shop"), QStringLiteral("orders")));
        QVERIFY(has(refs.relations, QString(), QStringLiteral("lines"))); // %TYPE and FROM.
        QVERIFY(has(refs.relations, QString(), QStringLiteral("refunds")));
        QVERIFY(has(refs.relations, QString(), QStringLiteral("invoices")));
        QVERIFY(has(refs.functions, QString(), QStringLiteral("notify_refund")));
        QVERIFY(has(refs.functions, QStringLiteral("shop"), QStringLiteral("is_open")));
        QVERIFY(refs.columns.contains(QStringLiteral("closed")));
        // What EXECUTE runs is only known when it runs.
        QVERIFY(!has(refs.relations, QString(), QStringLiteral("drafts")));
    }

    // libpg_query cannot look up a variable's type outside public, and
    // gives up on the whole function; the words still say a lot.
    void plpgsqlTheParserGivesUpOn()
    {
        const QByteArray body = R"sql(
DECLARE
    s shop.status;
BEGIN
    SELECT o.status INTO s FROM shop.orders o JOIN lines l ON l.order_id = o.id;
    PERFORM shop.notify(s);
END)sql";
        const BodyReferences refs
            = bodyReferences(QStringLiteral("plpgsql"), body,
                             "CREATE FUNCTION shop.f() RETURNS void LANGUAGE plpgsql AS $function$"
                                 + body + "$function$");
        QVERIFY(!refs.parsed);
        QVERIFY(!refs.error.isEmpty());
        QVERIFY(has(refs.relations, QStringLiteral("shop"), QStringLiteral("orders")));
        QVERIFY(has(refs.relations, QString(), QStringLiteral("lines")));
        QVERIFY(has(refs.functions, QStringLiteral("shop"), QStringLiteral("notify")));
    }

    void unreadableBody()
    {
        const BodyReferences refs = bodyReferences(QStringLiteral("sql"), "SELECT FROM WHERE oops");
        QVERIFY(!refs.parsed);
        QVERIFY(!refs.error.isEmpty());
    }

    void searchPath()
    {
        QCOMPARE(splitSearchPath(QStringLiteral("\"$user\", public"), QStringLiteral("ann")),
                 QStringList({QStringLiteral("ann"), QStringLiteral("public")}));
        QCOMPARE(splitSearchPath(QStringLiteral("Shop, \"Mixed\""), QStringLiteral("ann")),
                 QStringList({QStringLiteral("shop"), QStringLiteral("Mixed")}));
        QVERIFY(splitSearchPath(QStringLiteral("\"\""), QStringLiteral("ann")).value(0).isEmpty());
    }

    void kinds()
    {
        QVERIFY(hasDependencies(ObjectKind::Table));
        QVERIFY(hasDependencies(ObjectKind::Function));
        QVERIFY(hasDependencies(ObjectKind::Domain));
        QVERIFY(hasDependencies(ObjectKind::Constraint));
        // Nothing can depend on a trigger, and databases and roles are
        // tracked in pg_shdepend, which says nothing about what breaks.
        QVERIFY(!hasDependencies(ObjectKind::Trigger));
        QVERIFY(!hasDependencies(ObjectKind::Role));
        QVERIFY(dependencyQueries(ObjectKind::Role, 1).empty());
        // A relation's columns can be looked at one by one.
        QCOMPARE(dependencyQueries(ObjectKind::Table, 1).size(),
                 dependencyQueries(ObjectKind::Function, 1).size() + 1);
    }
};

QTEST_GUILESS_MAIN(TestDependencies)
#include "tst_dependencies.moc"
