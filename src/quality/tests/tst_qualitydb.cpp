// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/QueryRunner.h"
#include "quality/Report.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;
using namespace slonisko::quality;
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

// A little of what the report looks for.
const char *const Setup = R"sql(
DROP SCHEMA IF EXISTS report_test CASCADE;
CREATE SCHEMA report_test;
SET search_path = report_test;
CREATE TABLE customer (id integer PRIMARY KEY, name text);
CREATE TABLE orders (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    customer_id integer REFERENCES customer (id),
    created timestamptz);
ALTER TABLE orders ADD CONSTRAINT orders_customer_again
    FOREIGN KEY (customer_id) REFERENCES customer (id);
ALTER TABLE orders ADD CONSTRAINT positive CHECK (id > 0) NOT VALID;
CREATE INDEX orders_created ON orders (created);
CREATE INDEX orders_created_copy ON orders (created);
CREATE INDEX orders_created_covering ON orders (created) INCLUDE (customer_id);
CREATE INDEX orders_created_nulls_last ON orders (created DESC NULLS LAST);
CREATE INDEX customer_name ON customer (name);
CREATE INDEX customer_name_c ON customer (name COLLATE "C" text_pattern_ops);
CREATE FUNCTION touch() RETURNS trigger LANGUAGE plpgsql
    AS $$ BEGIN NEW.created := now(); RETURN NEW; END $$;
CREATE TRIGGER orders_touch BEFORE UPDATE ON orders FOR EACH ROW EXECUTE FUNCTION touch();
ALTER TABLE orders DISABLE TRIGGER orders_touch;
CREATE TABLE heap (line text) WITH (autovacuum_enabled = false);
CREATE SEQUENCE small AS smallint MAXVALUE 100;
SELECT setval('small', 95);
)sql";

const char *const Teardown = "DROP SCHEMA IF EXISTS report_test CASCADE";

const Topic *topic(const std::vector<Topic> &topics, const char *id)
{
    const auto it = std::ranges::find(topics, QString::fromUtf8(id), &Topic::id);
    return it == topics.end() ? nullptr : &*it;
}

bool found(const std::vector<Topic> &topics, const char *id, const QString &object)
{
    const Topic *t = topic(topics, id);
    return t && std::ranges::any_of(t->issues, [&](const Issue &i) {
               return i.object.contains(object);
           });
}

} // namespace

class TestReportDb : public QObject
{
    Q_OBJECT

private:
    Connection *m_connection = nullptr;
    QueryRunner *m_runner = nullptr;
    QByteArray m_conninfo;

    QueryOutcome run(QueryRunner *runner, const char *sql)
    {
        std::optional<QueryOutcome> outcome;
        runner->run(sql, this, [&outcome](const QueryOutcome &o) { outcome = o; });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 30'000); }();
        return outcome.value_or(QueryOutcome {});
    }

    Loaded load(bool health)
    {
        std::optional<Loaded> loaded;
        QString error;
        loadReport(m_runner, {{QStringLiteral("report_test")}, {}}, health, this,
                   [&](const Loaded &l, const QString &e) {
                       loaded = l;
                       error = e;
                   });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(loaded.has_value(), 30'000); }();
        [&] { QVERIFY2(error.isEmpty(), qPrintable(error)); }();
        return loaded.value_or(Loaded {});
    }

    int runTool(const QStringList &arguments, QByteArray *errors = nullptr)
    {
        QProcess process;
        process.start(QStringLiteral(SLONISKO_QUALITY_REPORT), arguments);
        if (!process.waitForFinished(120'000))
            return -1;
        if (errors)
            *errors = process.readAllStandardError();
        return process.exitCode();
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
        const QueryOutcome setup = run(m_runner, Setup);
        QVERIFY2(setup.ok(), qPrintable(setup.error));
    }

    void cleanupTestCase()
    {
        if (m_runner)
            run(m_runner, Teardown);
    }

    void catalogOnly()
    {
        const Loaded loaded = load(false);
        QVERIFY(!loaded.health);
        const std::vector<Topic> topics = analyse(loaded.model, nullptr);
        QVERIFY(found(topics, "duplicate-indexes", QStringLiteral("orders_created_copy")));
        // The one INCLUDing a column stays; the plain ones go.
        QVERIFY(found(topics, "duplicate-indexes", QStringLiteral("orders orders_created")));
        QVERIFY(!found(topics, "duplicate-indexes", QStringLiteral("orders_created_covering")));
        QVERIFY(!found(topics, "duplicate-indexes", QStringLiteral("orders_created_nulls_last")));
        QVERIFY(!found(topics, "duplicate-indexes", QStringLiteral("customer_name")));

        // What the comparison reads from the catalog.
        const docgen::Relation *orders
            = loaded.model.relation(QStringLiteral("report_test"), QStringLiteral("orders"));
        QVERIFY(orders);
        const auto index = [&](const char *name) {
            const auto it
                = std::ranges::find(orders->indexes, QString::fromUtf8(name), &docgen::Index::name);
            return it == orders->indexes.end() ? docgen::Index() : *it;
        };
        QCOMPARE(index("orders_created").keyOptions, std::vector<int> {0});
        QCOMPARE(index("orders_created").keyQualifiers, QStringList {QString()});
        QCOMPARE(index("orders_created_nulls_last").keyOptions, std::vector<int> {1});
        QCOMPARE(index("orders_created_covering").keyColumns, 1);
        QCOMPARE(index("orders_created_covering").columns,
                 (QStringList {QStringLiteral("created"), QStringLiteral("customer_id")}));
        const docgen::Relation *customer
            = loaded.model.relation(QStringLiteral("report_test"), QStringLiteral("customer"));
        QVERIFY(customer);
        const auto collated = std::ranges::find(
            customer->indexes, QStringLiteral("customer_name_c"), &docgen::Index::name);
        QVERIFY(collated != customer->indexes.end());
        QCOMPARE(collated->keyQualifiers,
                 QStringList {QStringLiteral("text_pattern_ops COLLATE C")});
        QVERIFY(found(topics, "duplicate-constraints", QStringLiteral("orders_customer")));
        QVERIFY(found(topics, "constraints-not-validated", QStringLiteral("positive")));
        QVERIFY(found(topics, "disabled-triggers", QStringLiteral("orders_touch")));
        QVERIFY(!found(topics, "foreign-keys-between-different-types", QStringLiteral("orders")));
        QVERIFY(found(topics, "tables-without-a-primary-key", QStringLiteral("heap")));
    }

    void health()
    {
        // A transaction left open in another session.
        Connection other;
        QueryRunner otherRunner(&other);
        other.open(m_conninfo);
        const QueryOutcome begun
            = run(&otherRunner, "BEGIN; SELECT pg_backend_pid(); SELECT count(*) FROM pg_class;");
        QVERIFY2(begun.ok(), qPrintable(begun.error));
        const int pid = begun.results.at(begun.results.size() - 2).value(0, 0).toInt();
        QVERIFY(pid > 0);

        const Loaded loaded = load(true);
        QVERIFY(loaded.health);
        const Health &h = *loaded.health;
        for (const Unavailable &u : h.unavailable)
            qWarning() << partTitle(u.part) << u.error;
        QVERIFY(h.unavailable.empty());
        QVERIFY(h.versionNumber >= 130000);
        QVERIFY(h.today.isValid());
        QVERIFY(h.monitor); // The container's postgres is a superuser.
        QVERIFY(h.maxConnections > 0);
        QVERIFY(h.connections >= 2);
        QVERIFY(h.setting(QStringLiteral("shared_buffers")));
        QCOMPARE(h.database.name, loaded.model.database);
        QVERIFY(h.database.size > 0);
        QVERIFY(std::ranges::any_of(
            h.databases, [&](const ServerDatabase &d) { return d.name == h.database.name; }));
        QVERIFY(std::ranges::any_of(h.tables, [](const TableHealth &t) {
            return t.name == QLatin1String("orders") && t.xidAge >= 0;
        }));
        QVERIFY(std::ranges::all_of(h.tables, [](const TableHealth &t) {
            return t.schema == QLatin1String("report_test");
        }));
        QVERIFY(std::ranges::any_of(h.indexes, [](const IndexHealth &i) {
            return i.name == QLatin1String("orders_pkey") && i.enforcing;
        }));
        QVERIFY(std::ranges::any_of(h.indexes, [](const IndexHealth &i) {
            return i.name == QLatin1String("orders_created") && !i.enforcing;
        }));
        QVERIFY(std::ranges::any_of(h.sessions, [pid](const Session &s) { return s.pid == pid; }));
        QVERIFY(!h.routinesChecked); // No plpgsql_check in the image.

        Thresholds now;
        now.idleInTransactionSeconds = 0;
        now.statisticsAgeSeconds = 0;
        now.unusedIndexBytes = 0;
        const std::vector<Topic> topics = analyse(loaded.model, &h, now);
        QVERIFY(found(topics, "transactions", QStringLiteral("pid %1,").arg(pid)));
        QVERIFY(found(topics, "sequences", QStringLiteral("report_test.small")));
        QCOMPARE(topic(topics, "sequences")->issues.front().severity, Severity::Critical);
        QVERIFY(found(topics, "autovacuum-off", QStringLiteral("report_test.heap")));
        QVERIFY(found(topics, "unused-indexes", QStringLiteral("orders_created")));
        QVERIFY(!found(topics, "unused-indexes", QStringLiteral("orders_pkey")));
        QVERIFY(
            topic(topics, "routine-errors")->unavailable.contains(QLatin1String("plpgsql_check")));

        run(&otherRunner, "ROLLBACK");
    }

    void commandLine()
    {
        QTemporaryDir directory;
        QByteArray errors;
        const QStringList common {QString::fromUtf8(m_conninfo), QStringLiteral("-n"),
                                  QStringLiteral("report_test"), QStringLiteral("-o"),
                                  directory.path()};
        // The nearly used up sequence is critical.
        QCOMPARE(
            runTool(QStringList {QStringLiteral("markdown")} + common
                        + QStringList {QStringLiteral("--fail-on"), QStringLiteral("critical")},
                    &errors),
            3);
        QVERIFY2(errors.contains(" critical, "), errors.constData());
        for (const char *page : {"index.md", "schema.md", "indexes.md", "dependencies.md",
                                 "duplicates.md", "health.md", "statements.md"})
            QVERIFY2(QFile::exists(directory.filePath(QString::fromUtf8(page))), page);
        QFile indexes(directory.filePath(QStringLiteral("indexes.md")));
        QVERIFY(indexes.open(QIODevice::ReadOnly));
        QVERIFY(indexes.readAll().contains("orders\\_created\\_copy"));

        // Without the statistics, nothing in the catalog is critical.
        QCOMPARE(
            runTool(QStringList {QStringLiteral("html"), QStringLiteral("-q"),
                                 QStringLiteral("--no-health")}
                        + common
                        + QStringList {QStringLiteral("--fail-on"), QStringLiteral("critical")},
                    &errors),
            0);
        QVERIFY2(errors.isEmpty(), errors.constData());
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("index.html"))));

        QCOMPARE(runTool({QStringLiteral("html"), QStringLiteral("--fail-on"),
                          QStringLiteral("sometimes")}),
                 2);
        QCOMPARE(runTool({QStringLiteral("pdf")}), 2);
        QCOMPARE(
            runTool({QStringLiteral("markdown"), QStringLiteral("--css"), QStringLiteral("x.css")}),
            2);
    }
};

QTEST_GUILESS_MAIN(TestReportDb)
#include "tst_qualitydb.moc"
