// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Generator.h"
#include "pg/QueryRunner.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko::docgen;
using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;
using slonisko::pg::QueryRunner;

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
DROP SCHEMA IF EXISTS docgen_test CASCADE;
DROP SCHEMA IF EXISTS docgen_other CASCADE;
CREATE SCHEMA docgen_test;
COMMENT ON SCHEMA docgen_test IS 'A shop.';
CREATE SCHEMA docgen_other;
SET search_path = docgen_test;
CREATE EXTENSION pg_trgm SCHEMA docgen_test;
CREATE TYPE status AS ENUM ('new', 'paid', 'sent');
CREATE TYPE money_range AS RANGE (subtype = numeric);
CREATE TYPE address AS (street text, city text);
CREATE DOMAIN positive AS integer NOT NULL DEFAULT 1 CONSTRAINT positive_check CHECK (VALUE > 0);
CREATE TABLE customer (
    id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL,
    code char(3),
    "Upper Case" text);
COMMENT ON TABLE customer IS 'People who buy.';
COMMENT ON COLUMN customer.name IS 'As they spell it.';
CREATE TABLE orders (
    id bigserial PRIMARY KEY,
    customer_id integer NOT NULL REFERENCES customer (id) ON DELETE CASCADE,
    state status NOT NULL DEFAULT 'new',
    total numeric(10, 2),
    doubled numeric GENERATED ALWAYS AS (total * 2) STORED,
    quantity positive);
CREATE INDEX orders_state ON orders (state) WHERE state <> 'sent';
CREATE INDEX orders_total ON orders (total) INCLUDE (state);
CREATE TABLE measurement (at date NOT NULL, value int) PARTITION BY RANGE (at);
CREATE TABLE measurement_2026 PARTITION OF measurement
    FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
CREATE VIEW big_orders AS SELECT id, total FROM orders WHERE total > 100;
CREATE MATERIALIZED VIEW order_totals AS SELECT customer_id, sum(total) FROM orders GROUP BY 1;
CREATE FUNCTION touch() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
COMMENT ON FUNCTION touch() IS 'Keeps the time.';
CREATE TRIGGER orders_touch BEFORE UPDATE ON orders FOR EACH ROW EXECUTE FUNCTION touch();
CREATE PROCEDURE archive(days integer) LANGUAGE sql AS $$ SELECT 1 $$;
CREATE AGGREGATE total_of(numeric) (sfunc = numeric_add, stype = numeric);
ALTER TABLE customer ENABLE ROW LEVEL SECURITY;
CREATE POLICY own_rows ON customer FOR SELECT USING (name = current_user);
CREATE SEQUENCE free_counter START 10 INCREMENT 5;
CREATE TABLE docgen_other.elsewhere (id int);
)sql";

const char *const Teardown
    = "DROP SCHEMA IF EXISTS docgen_test CASCADE; DROP SCHEMA IF EXISTS docgen_other CASCADE";

template<typename T>
const T *named(const std::vector<T> &objects, const char *name)
{
    const auto it = std::ranges::find(objects, QString::fromUtf8(name), &T::name);
    return it == objects.end() ? nullptr : &*it;
}

} // namespace

class TestDocgenModel : public QObject
{
    Q_OBJECT

private:
    Connection *m_connection = nullptr;
    QueryRunner *m_runner = nullptr;
    QByteArray m_conninfo;

    QueryOutcome run(const char *sql)
    {
        std::optional<QueryOutcome> outcome;
        m_runner->run(sql, this, [&outcome](const QueryOutcome &o) { outcome = o; });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 30'000); }();
        return outcome.value_or(QueryOutcome {});
    }

    Model load(const SchemaFilter &filter)
    {
        std::optional<Model> model;
        QString error;
        loadModel(m_runner, filter, this, [&](const Model &m, const QString &e) {
            model = m;
            error = e;
        });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(model.has_value(), 30'000); }();
        [&] { QVERIFY2(error.isEmpty(), qPrintable(error)); }();
        return model.value_or(Model {});
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_conninfo = serverConninfo();
        if (m_conninfo.isEmpty())
            QSKIP("No test server; set SLONISKO_TEST_CONNINFO or run through ctest");
        m_connection = new Connection(this);
        m_runner = new QueryRunner(m_connection, this);
        m_connection->open(m_conninfo);
        const QueryOutcome setup = run(Setup);
        QVERIFY2(setup.ok(), qPrintable(setup.error));
    }

    void cleanupTestCase()
    {
        if (m_runner)
            run(Teardown);
    }

    void schemas()
    {
        const Model model = load({{QStringLiteral("docgen_*")}, {}});
        QCOMPARE(model.schemas.size(), std::size_t(2));
        QCOMPARE(model.schemas[1].name, QStringLiteral("docgen_test"));
        QCOMPARE(model.schemas[1].comment, QStringLiteral("A shop."));
        QVERIFY(!model.serverVersion.isEmpty());

        const Model one = load({{QStringLiteral("docgen_*")}, {QStringLiteral("docgen_o?her")}});
        QCOMPARE(one.schemas.size(), std::size_t(1));
        QVERIFY(!one.relation(QStringLiteral("docgen_other"), QStringLiteral("elsewhere")));

        // An underscore is not a wildcard: docgenXtest would not match either.
        QCOMPARE(load({{QStringLiteral("docgen_tes_")}, {}}).schemas.size(), std::size_t(0));
    }

    void tables()
    {
        const Model model = load({{QStringLiteral("docgen_test")}, {}});
        const Relation *customer
            = model.relation(QStringLiteral("docgen_test"), QStringLiteral("customer"));
        QVERIFY(customer);
        QCOMPARE(customer->comment, QStringLiteral("People who buy."));
        QVERIFY(customer->rowSecurity);
        QCOMPARE(customer->columns.size(), std::size_t(4));
        QCOMPARE(customer->columns[0].identity, QStringLiteral("ALWAYS"));
        QCOMPARE(customer->columns[1].comment, QStringLiteral("As they spell it."));
        QVERIFY(customer->columns[1].notNull);
        QCOMPARE(customer->columns[2].type, QStringLiteral("character(3)"));
        QCOMPARE(customer->columns[3].name, QStringLiteral("Upper Case"));
        QVERIFY(customer->primaryKey());
        QCOMPARE(customer->primaryKey()->columns, QStringList {QStringLiteral("id")});
        QCOMPARE(customer->policies.size(), std::size_t(1));
        QCOMPARE(customer->policies[0].command, QStringLiteral("SELECT"));
        QCOMPARE(customer->policies[0].roles, QStringList {QStringLiteral("public")});

        const Relation *orders
            = model.relation(QStringLiteral("docgen_test"), QStringLiteral("orders"));
        QVERIFY(orders);
        QVERIFY(orders->columns[0].defaultValue.startsWith(QLatin1String("nextval(")));
        QCOMPARE(orders->columns[2].defaultValue, QStringLiteral("'new'::status"));
        QCOMPARE(orders->columns[4].generated, QStringLiteral("(total * (2)::numeric)"));
        QVERIFY(!orders->columns[4].virtualGenerated);
        QVERIFY(orders->columns[4].defaultValue.isEmpty());
        QCOMPARE(orders->columns[5].type, QStringLiteral("positive"));

        const Constraint *fk = named(orders->constraints, "orders_customer_id_fkey");
        QVERIFY(fk);
        QCOMPARE(fk->type, 'f');
        QCOMPARE(fk->referencedOid, customer->oid);
        QCOMPARE(fk->referencedTable, QStringLiteral("customer"));
        QCOMPARE(fk->referencedColumns, QStringList {QStringLiteral("id")});
        QCOMPARE(fk->onDelete, QStringLiteral("CASCADE"));
        QCOMPARE(fk->onUpdate, QString());
        // NOT NULL constraints are the columns' business.
        QVERIFY(std::ranges::none_of(orders->constraints,
                                     [](const Constraint &c) { return c.type == 'n'; }));

        const Index *state = named(orders->indexes, "orders_state");
        QVERIFY(state);
        QVERIFY(state->partial);
        QCOMPARE(state->columns, QStringList {QStringLiteral("state")});
        const Index *total = named(orders->indexes, "orders_total");
        QCOMPARE(total->columns, (QStringList {QStringLiteral("total"), QStringLiteral("state")}));
        QCOMPARE(total->keyColumns, 1);
        QVERIFY(named(orders->indexes, "orders_pkey")->primary);

        QCOMPARE(orders->triggers.size(), std::size_t(1));
        QCOMPARE(orders->triggers[0].function, QStringLiteral("docgen_test.touch"));
        QVERIFY(orders->triggers[0].definition.startsWith(
            QLatin1String("CREATE TRIGGER orders_touch")));

        const Relation *measurement
            = model.relation(QStringLiteral("docgen_test"), QStringLiteral("measurement"));
        QCOMPARE(measurement->kind, 'p');
        QCOMPARE(measurement->partitionKey, QStringLiteral("RANGE (at)"));
        const Relation *partition
            = model.relation(QStringLiteral("docgen_test"), QStringLiteral("measurement_2026"));
        QVERIFY(partition->isPartition());
        QCOMPARE(partition->parentName, QStringLiteral("measurement"));
        QVERIFY(
            partition->partitionBound.startsWith(QLatin1String("FOR VALUES FROM ('2026-01-01')")));

        const Relation *view
            = model.relation(QStringLiteral("docgen_test"), QStringLiteral("big_orders"));
        QCOMPARE(view->kind, 'v');
        QVERIFY(view->definition.contains(QLatin1String("total > 100")));
        QCOMPARE(view->columns.size(), std::size_t(2));
        QVERIFY(std::ranges::any_of(model.dependencies, [&](const Dependency &d) {
            return d.view == view->oid && d.relation == orders->oid;
        }));
        QCOMPARE(
            model.relation(QStringLiteral("docgen_test"), QStringLiteral("order_totals"))->kind,
            'm');
    }

    void otherObjects()
    {
        const Model model = load({{QStringLiteral("docgen_test")}, {}});

        // pg_trgm's functions and types are in the schema, but they are its.
        QVERIFY(!named(model.routines, "similarity"));
        QVERIFY(!named(model.types, "gtrgm"));
        QVERIFY(named(model.extensions, "pg_trgm"));

        const Routine *touch = named(model.routines, "touch");
        QVERIFY(touch);
        QCOMPARE(touch->kind, 'f');
        QCOMPARE(touch->result, QStringLiteral("trigger"));
        QCOMPARE(touch->language, QStringLiteral("plpgsql"));
        QCOMPARE(touch->body, QStringLiteral(" BEGIN RETURN NEW; END "));
        QCOMPARE(touch->comment, QStringLiteral("Keeps the time."));
        QVERIFY(touch->definition.startsWith(
            QLatin1String("CREATE OR REPLACE FUNCTION docgen_test.touch()")));
        const Routine *archive = named(model.routines, "archive");
        QCOMPARE(archive->kind, 'p');
        QCOMPARE(archive->arguments, QStringLiteral("IN days integer"));
        const Routine *aggregate = named(model.routines, "total_of");
        QCOMPARE(aggregate->kind, 'a');
        QVERIFY(aggregate->definition.isEmpty());

        const UserType *status = named(model.types, "status");
        QCOMPARE(status->kind, 'e');
        QCOMPARE(
            status->labels,
            (QStringList {QStringLiteral("new"), QStringLiteral("paid"), QStringLiteral("sent")}));
        const UserType *range = named(model.types, "money_range");
        QCOMPARE(range->kind, 'r');
        QCOMPARE(range->baseType, QStringLiteral("numeric"));
        const UserType *address = named(model.types, "address");
        QCOMPARE(address->kind, 'c');
        QCOMPARE(address->attributes.size(), std::size_t(2));
        QCOMPARE(address->attributes[1].name, QStringLiteral("city"));
        const UserType *positive = named(model.types, "positive");
        QCOMPARE(positive->kind, 'd');
        QCOMPARE(positive->baseType, QStringLiteral("integer"));
        QVERIFY(positive->notNull);
        QCOMPARE(positive->defaultValue, QStringLiteral("1"));
        QCOMPARE(positive->constraints.size(), std::size_t(1));
        QCOMPARE(positive->constraints[0].definition, QStringLiteral("CHECK (VALUE > 0)"));
        // A table's own row type is not one to list.
        QVERIFY(!named(model.types, "customer"));

        const Sequence *serial = named(model.sequences, "orders_id_seq");
        QVERIFY(serial);
        QCOMPARE(serial->ownedBy, QStringLiteral("docgen_test.orders.id"));
        QCOMPARE(serial->type, QStringLiteral("bigint"));
        const Sequence *identity = named(model.sequences, "customer_id_seq");
        QCOMPARE(identity->ownedBy, QStringLiteral("docgen_test.customer.id"));
        const Sequence *free = named(model.sequences, "free_counter");
        QCOMPARE(free->start, 10);
        QCOMPARE(free->increment, 5);
        QVERIFY(free->ownedBy.isEmpty());
    }

    void commandLine_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("entry");
        QTest::newRow("html") << QStringLiteral("html") << QStringLiteral("index.html");
        QTest::newRow("markdown") << QStringLiteral("markdown") << QStringLiteral("index.md");
        QTest::newRow("asciidoc") << QStringLiteral("asciidoc") << QStringLiteral("index.adoc");
        QTest::newRow("antora") << QStringLiteral("antora") << QStringLiteral("antora.yml");
        QTest::newRow("sphinx") << QStringLiteral("sphinx") << QStringLiteral("conf.py");
    }

    void commandLine()
    {
        QFETCH(QString, format);
        QFETCH(QString, entry);
        QTemporaryDir directory;
        QProcess process;
        process.start(QStringLiteral(SLONISKO_DOCGEN),
                      {format, QString::fromUtf8(m_conninfo), QStringLiteral("-n"),
                       QStringLiteral("docgen_test"), QStringLiteral("-o"), directory.path(),
                       QStringLiteral("--title"), QStringLiteral("Shop: the database")});
        QVERIFY(process.waitForFinished(120'000));
        const QByteArray errors = process.readAllStandardError();
        QVERIFY2(process.exitCode() == 0, errors.constData());
        QVERIFY2(QFile::exists(directory.filePath(entry)), errors.constData());
        QVERIFY(errors.contains(" pages; start at "));

        const QString root
            = format == QLatin1String("antora") ? QStringLiteral("modules/ROOT/pages/") : QString();
        const QString suffix = format == QLatin1String("html") ? QStringLiteral(".html")
            : format == QLatin1String("markdown")              ? QStringLiteral(".md")
            : format == QLatin1String("sphinx")                ? QStringLiteral(".rst")
                                                               : QStringLiteral(".adoc");
        QFile orders(
            directory.filePath(root + QStringLiteral("docgen_test/tables/orders") + suffix));
        QVERIFY(orders.open(QIODevice::ReadOnly));
        const QByteArray text = orders.readAll();
        QVERIFY(text.contains("customer_id"));
        QVERIFY(text.contains("orders_touch"));
        QVERIFY(!QFile::exists(directory.filePath(root + QStringLiteral("checks") + suffix)));
        // The partition is on its parent's page only.
        QVERIFY(!QFile::exists(directory.filePath(
            root + QStringLiteral("docgen_test/tables/measurement_2026") + suffix)));
    }
};

QTEST_GUILESS_MAIN(TestDocgenModel)
#include "tst_docgenmodel.moc"
