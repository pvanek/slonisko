// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Checks.h"

#include <QTest>

using namespace slonisko::docgen;
using namespace slonisko::quality;

namespace {

Column column(const char *name, const char *type, bool notNull = false)
{
    Column c;
    c.name = QString::fromUtf8(name);
    c.type = QString::fromUtf8(type);
    c.notNull = notNull;
    return c;
}

Index index(const char *name, QStringList columns, bool unique = false, bool primary = false)
{
    Index i;
    i.name = QString::fromUtf8(name);
    i.method = QStringLiteral("btree");
    i.keyColumns = int(columns.size());
    i.columns = std::move(columns);
    i.unique = unique;
    i.primary = primary;
    i.definition = QStringLiteral("CREATE INDEX %1 ON shop.x USING btree (...)").arg(i.name);
    return i;
}

// A small shop with a little of everything the checks look for.
Model shop()
{
    Model model;
    model.database = QStringLiteral("shopdb");
    model.serverVersion = QStringLiteral("18.0");
    model.schemas.push_back(
        {QStringLiteral("shop"), QStringLiteral("owner"), QStringLiteral("Where the money is.")});

    Relation customer;
    customer.oid = 100;
    customer.schema = QStringLiteral("shop");
    customer.name = QStringLiteral("customer");
    customer.comment = QStringLiteral("People who buy.\n\nAnd sometimes return.");
    customer.columns
        = {column("id", "integer", true), column("name", "text"), column("code", "character(3)")};
    Constraint customerKey;
    customerKey.name = QStringLiteral("customer_pkey");
    customerKey.type = 'p';
    customerKey.columns = {QStringLiteral("id")};
    customerKey.definition = QStringLiteral("PRIMARY KEY (id)");
    customer.constraints = {customerKey};
    customer.indexes = {index("customer_pkey", {QStringLiteral("id")}, true, true)};

    Relation orders;
    orders.oid = 200;
    orders.schema = QStringLiteral("shop");
    orders.name = QStringLiteral("orders");
    orders.comment = QStringLiteral("Orders | with a pipe and *stars*.");
    orders.columns = {column("id", "bigint", true), column("customer_id", "bigint", true),
                      column("created", "timestamp without time zone")};
    orders.columns[0].defaultValue = QStringLiteral("nextval('shop.orders_id_seq'::regclass)");
    Constraint ordersKey = customerKey;
    ordersKey.name = QStringLiteral("orders_pkey");
    Constraint toCustomer;
    toCustomer.name = QStringLiteral("orders_customer_id_fkey");
    toCustomer.type = 'f';
    toCustomer.columns = {QStringLiteral("customer_id")};
    toCustomer.referencedOid = 100;
    toCustomer.referencedSchema = QStringLiteral("shop");
    toCustomer.referencedTable = QStringLiteral("customer");
    toCustomer.referencedColumns = {QStringLiteral("id")};
    toCustomer.onDelete = QStringLiteral("CASCADE");
    toCustomer.definition
        = QStringLiteral("FOREIGN KEY (customer_id) REFERENCES shop.customer(id)");
    orders.constraints = {ordersKey, toCustomer};
    orders.indexes = {index("orders_pkey", {QStringLiteral("id")}, true, true),
                      index("orders_id_idx", {QStringLiteral("id")})};
    orders.triggers = {{QStringLiteral("orders_touch"),
                        QStringLiteral("shop.touch"),
                        true,
                        QStringLiteral("CREATE TRIGGER orders_touch BEFORE UPDATE ON shop.orders "
                                       "FOR EACH ROW EXECUTE FUNCTION shop.touch()"),
                        {}}};

    Relation log;
    log.oid = 300;
    log.schema = QStringLiteral("shop");
    log.name = QStringLiteral("log");
    log.columns = {column("line", "text")};

    // Two names a case-insensitive file system cannot tell apart.
    Relation data = log;
    data.oid = 400;
    data.name = QStringLiteral("Data");
    Relation data2 = log;
    data2.oid = 401;
    data2.name = QStringLiteral("data");

    Relation view;
    view.oid = 500;
    view.schema = QStringLiteral("shop");
    view.name = QStringLiteral("Big Orders");
    view.kind = 'v';
    view.columns = {column("id", "bigint")};
    view.definition = QStringLiteral("SELECT id FROM shop.orders WHERE id > 100;");

    model.relations = {customer, data, data2, log, orders, view};
    model.dependencies = {{500, 200}};

    Routine touch;
    touch.oid = 600;
    touch.schema = QStringLiteral("shop");
    touch.name = QStringLiteral("touch");
    touch.result = QStringLiteral("trigger");
    touch.language = QStringLiteral("plpgsql");
    touch.comment = QStringLiteral("Keeps the time.");
    touch.definition = QStringLiteral("CREATE FUNCTION shop.touch() RETURNS trigger LANGUAGE "
                                      "plpgsql AS $$ BEGIN RETURN NEW; END $$");
    model.routines = {touch};

    UserType status;
    status.oid = 700;
    status.schema = QStringLiteral("shop");
    status.name = QStringLiteral("status");
    status.kind = 'e';
    status.labels = {QStringLiteral("new"), QStringLiteral("paid")};
    model.types = {status};

    Sequence sequence;
    sequence.schema = QStringLiteral("shop");
    sequence.name = QStringLiteral("orders_id_seq");
    sequence.type = QStringLiteral("bigint");
    sequence.ownedBy = QStringLiteral("shop.orders.id");
    model.sequences = {sequence};
    return model;
}

const Finding *find(const std::vector<Finding> &findings, Check check, const QString &object = {})
{
    const auto it = std::ranges::find_if(findings, [&](const Finding &f) {
        return f.check == check && (object.isEmpty() || f.object == object || f.relation == object);
    });
    return it == findings.end() ? nullptr : &*it;
}

} // namespace

class TestChecks : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void checks()
    {
        const std::vector<Finding> findings = runChecks(shop());

        QVERIFY(find(findings, Check::NoPrimaryKey, QStringLiteral("log")));
        QVERIFY(!find(findings, Check::NoPrimaryKey, QStringLiteral("orders")));

        const Finding *unindexed
            = find(findings, Check::UnindexedForeignKey, QStringLiteral("orders_customer_id_fkey"));
        QVERIFY(unindexed);
        QCOMPARE(unindexed->fix, QStringLiteral("CREATE INDEX ON shop.orders (customer_id);"));

        const Finding *mismatch = find(findings, Check::ForeignKeyTypeMismatch);
        QVERIFY(mismatch);
        QVERIFY(mismatch->message.contains(QLatin1String("bigint")));
        QVERIFY(mismatch->message.contains(QLatin1String("integer")));

        // The unique one stays, the plain copy of it goes.
        const Finding *duplicate = find(findings, Check::DuplicateIndex);
        QVERIFY(duplicate);
        QCOMPARE(duplicate->object, QStringLiteral("orders_id_idx"));
        QCOMPARE(duplicate->fix, QStringLiteral("DROP INDEX shop.orders_id_idx;"));
        QCOMPARE(std::ranges::count(findings, Check::DuplicateIndex, &Finding::check), 1);
        QVERIFY(!find(findings, Check::RedundantIndex));

        // Data and data, and both need quoting, as does Big Orders.
        QVERIFY(find(findings, Check::NameCaseClash, QStringLiteral("data")));
        QVERIFY(find(findings, Check::NeedsQuoting, QStringLiteral("Data")));
        QVERIFY(find(findings, Check::NeedsQuoting, QStringLiteral("Big Orders")));
        QVERIFY(!find(findings, Check::NeedsQuoting, QStringLiteral("orders")));
        // The sequence is a serial's.
        QVERIFY(!find(findings, Check::UnusedSequence));
        QVERIFY(!find(findings, Check::SequenceTypeMismatch));

        QVERIFY(find(findings, Check::DiscouragedType, QStringLiteral("code")));
        QVERIFY(find(findings, Check::DiscouragedType, QStringLiteral("created")));
        QVERIFY(find(findings, Check::DiscouragedType, QStringLiteral("id"))); // serial
        QVERIFY(find(findings, Check::Undocumented, QStringLiteral("log")));
        QVERIFY(!find(findings, Check::Undocumented, QStringLiteral("customer")));
        QVERIFY(!find(findings, Check::Undocumented, QStringLiteral("touch()")));

        // Grouped by check, in the report's order.
        for (std::size_t i = 1; i < findings.size(); ++i)
            QVERIFY(int(findings[i - 1].check) <= int(findings[i].check));
    }

    void foreignKeyCoveredByIndex()
    {
        Model model = shop();
        Relation &orders = model.relations[4];
        QCOMPARE(orders.name, QStringLiteral("orders"));
        // Leading columns in another order still serve the key.
        orders.indexes.push_back(
            index("orders_customer", {QStringLiteral("customer_id"), QStringLiteral("id")}));
        QVERIFY(!find(runChecks(model), Check::UnindexedForeignKey));
        // A partial index does not.
        orders.indexes.back().partial = true;
        QVERIFY(find(runChecks(model), Check::UnindexedForeignKey));
    }

    void moreChecks()
    {
        Model model = shop();
        Relation &orders = model.relations[4];
        Relation &log = model.relations[3];
        QCOMPARE(orders.name, QStringLiteral("orders"));
        QCOMPARE(log.name, QStringLiteral("log"));

        // A prefix of another index.
        orders.indexes.push_back(
            index("orders_customer", {QStringLiteral("customer_id"), QStringLiteral("id")}));
        orders.indexes.push_back(index("orders_customer_only", {QStringLiteral("customer_id")}));
        // The foreign key again, under another name, and a check never validated.
        Constraint again = orders.constraints[1];
        again.name = QStringLiteral("orders_customer_again");
        orders.constraints.push_back(again);
        Constraint positive;
        positive.name = QStringLiteral("positive");
        positive.type = 'c';
        positive.definition = QStringLiteral("CHECK (id > 0) NOT VALID");
        orders.constraints.push_back(positive);
        // The trigger again, disabled.
        Trigger copy = orders.triggers.front();
        copy.name = QStringLiteral("orders_touch2");
        copy.definition.replace(QLatin1String("orders_touch"), QLatin1String("orders_touch2"));
        copy.enabled = false;
        orders.triggers.push_back(copy);
        // Row level security on without a policy.
        log.rowSecurity = true;
        // A copy of a view, and of a routine's body, under other names.
        Relation view = model.relations[5];
        view.oid = 501;
        view.name = QStringLiteral("big_orders_again");
        view.definition = QStringLiteral("SELECT id\n   FROM shop.orders WHERE id > 100;");
        model.relations.push_back(view);
        Routine definer = model.routines.front();
        definer.oid = 601;
        definer.name = QStringLiteral("whoami");
        definer.result = QStringLiteral("text");
        definer.language = QStringLiteral("sql");
        definer.securityDefiner = true;
        definer.body = QStringLiteral("SELECT current_user || ' is who runs this, really'");
        definer.definition = QStringLiteral("CREATE FUNCTION shop.whoami() ... AS $$")
            + definer.body + QStringLiteral("$$");
        Routine twin = definer;
        twin.oid = 602;
        twin.name = QStringLiteral("whoami2");
        twin.securityDefiner = false;
        model.routines.push_back(definer);
        model.routines.push_back(twin);
        // A sequence nothing uses, and one narrower than its column.
        Sequence lonely;
        lonely.schema = QStringLiteral("shop");
        lonely.name = QStringLiteral("lonely");
        lonely.type = QStringLiteral("bigint");
        model.sequences.push_back(lonely);
        model.sequences.front().type = QStringLiteral("integer");

        const std::vector<Finding> findings = runChecks(model);
        const Finding *redundant = find(findings, Check::RedundantIndex);
        QVERIFY(redundant);
        QCOMPARE(redundant->object, QStringLiteral("orders_customer_only"));

        const Finding *duplicate = find(findings, Check::DuplicateConstraint);
        QVERIFY(duplicate);
        QCOMPARE(duplicate->object, QStringLiteral("orders_customer_again"));
        QCOMPARE(duplicate->fix,
                 QStringLiteral("ALTER TABLE shop.orders DROP CONSTRAINT orders_customer_again;"));

        const Finding *notValid = find(findings, Check::NotValidConstraint);
        QVERIFY(notValid);
        QCOMPARE(notValid->fix,
                 QStringLiteral("ALTER TABLE shop.orders VALIDATE CONSTRAINT positive;"));

        QVERIFY(find(findings, Check::DisabledTrigger, QStringLiteral("orders_touch2")));
        const Finding *twice = find(findings, Check::DuplicateTrigger);
        QVERIFY(twice);
        QCOMPARE(twice->object, QStringLiteral("orders_touch2"));

        QVERIFY(find(findings, Check::RowSecurity, QStringLiteral("log")));

        const Finding *definerFinding = find(findings, Check::SecurityDefiner);
        QVERIFY(definerFinding);
        QCOMPARE(definerFinding->object, QStringLiteral("whoami()"));
        QCOMPARE(definerFinding->fix,
                 QStringLiteral("ALTER FUNCTION shop.whoami() SET search_path = shop, pg_temp;"));

        QVERIFY(find(findings, Check::IdenticalDefinition, QStringLiteral("big_orders_again")));
        QVERIFY(find(findings, Check::IdenticalDefinition, QStringLiteral("whoami2()")));
        QVERIFY(!find(findings, Check::IdenticalDefinition, QStringLiteral("whoami()")));

        QVERIFY(find(findings, Check::UnusedSequence, QStringLiteral("lonely")));
        QVERIFY(!find(findings, Check::UnusedSequence, QStringLiteral("orders_id_seq")));
        const Finding *narrow = find(findings, Check::SequenceTypeMismatch);
        QVERIFY(narrow);
        QCOMPARE(narrow->fix, QStringLiteral("ALTER SEQUENCE shop.orders_id_seq AS bigint;"));
    }

    // What an index can do, not only its column names: the columns it
    // INCLUDEs, the order it sorts in and its operator classes.
    void indexesAlike()
    {
        const auto findingsOn = [](std::vector<Index> indexes) {
            Model model;
            Relation table;
            table.oid = 1;
            table.schema = QStringLiteral("s");
            table.name = QStringLiteral("t");
            table.comment = QStringLiteral("A table.");
            table.indexes = std::move(indexes);
            model.relations = {table};
            std::vector<Finding> findings = runChecks(model);
            std::erase_if(findings, [](const Finding &f) {
                return f.check != Check::DuplicateIndex && f.check != Check::RedundantIndex;
            });
            return findings;
        };
        const auto included = [](Index i, QStringList columns) {
            i.columns += columns;
            return i;
        };
        const auto ordered = [](Index i, std::vector<int> options) {
            i.keyOptions = std::move(options);
            return i;
        };
        const QString x = QStringLiteral("x");
        const QString y = QStringLiteral("y");
        const QString t = QStringLiteral("t");

        // The plain one goes, not the one also INCLUDing t.
        std::vector<Finding> f
            = findingsOn({index("plain", {x}), included(index("covering", {x}), {t})});
        QCOMPARE(f.size(), std::size_t(1));
        QCOMPARE(f.front().check, Check::DuplicateIndex);
        QCOMPARE(f.front().object, QStringLiteral("plain"));
        QVERIFY(f.front().message.contains(QLatin1String("INCLUDE t")));
        // Unless only the plain one is unique.
        QVERIFY(
            findingsOn({index("plain", {x}, true), included(index("covering", {x}), {t})}).empty());
        // Different columns INCLUDEd: each serves index-only scans the other cannot.
        QVERIFY(
            findingsOn({included(index("with_t", {x}), {t}), included(index("with_y", {x}), {y})})
                .empty());
        // A key column does what an INCLUDEd one does.
        f = findingsOn({included(index("x_with_t", {x}), {t}), index("x_t", {x, t})});
        QCOMPARE(f.size(), std::size_t(1));
        QCOMPARE(f.front().check, Check::RedundantIndex);
        QCOMPARE(f.front().object, QStringLiteral("x_with_t"));
        QVERIFY(findingsOn({included(index("x_with_t", {x}), {t}), index("x_y", {x, y})}).empty());

        // DESC is ASC read backwards: DESC NULLS FIRST, the default.
        f = findingsOn({index("up", {x}), ordered(index("down", {x}), {3})});
        QCOMPARE(f.size(), std::size_t(1));
        QCOMPARE(f.front().object, QStringLiteral("up"));
        // DESC NULLS LAST is not.
        QVERIFY(findingsOn({index("up", {x}), ordered(index("down", {x}), {1})}).empty());
        // Nor is one column turned of two.
        QVERIFY(findingsOn({index("up", {x, y}), ordered(index("mixed", {x, y}), {0, 3})}).empty());
        // Both turned is the same index read backwards.
        QCOMPARE(
            findingsOn({ordered(index("a", {x, y}), {0, 3}), ordered(index("b", {x, y}), {3, 0})})
                .size(),
            std::size_t(1));
        // A prefix read backwards serves too.
        f = findingsOn({ordered(index("down", {x}), {3}), index("xy", {x, y})});
        QCOMPARE(f.size(), std::size_t(1));
        QCOMPARE(f.front().check, Check::RedundantIndex);
        QVERIFY(findingsOn({ordered(index("down", {x}), {1}), index("xy", {x, y})}).empty());
        // Backwards is a btree's; a hash index has no order to turn.
        Index hash = ordered(index("h", {x}), {3});
        hash.method = QStringLiteral("hash");
        Index hash2 = index("h2", {x});
        hash2.method = QStringLiteral("hash");
        QVERIFY(findingsOn({hash, hash2}).empty());

        // Another operator class or collation compares differently.
        Index pattern = index("pattern", {t});
        pattern.keyQualifiers = {QStringLiteral("text_pattern_ops")};
        QVERIFY(findingsOn({index("plain", {t}), pattern}).empty());
        Index collated = index("c", {t});
        collated.keyQualifiers = {QStringLiteral("COLLATE C")};
        QVERIFY(findingsOn({index("plain", {t}), collated}).empty());
    }
};

QTEST_MAIN(TestChecks)
#include "tst_checks.moc"
