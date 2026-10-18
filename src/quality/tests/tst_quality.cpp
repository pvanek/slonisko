// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Report.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;
using namespace slonisko::quality;

namespace {

docgen::Column column(const char *name, const char *type)
{
    docgen::Column c;
    c.name = QString::fromUtf8(name);
    c.type = QString::fromUtf8(type);
    return c;
}

// A table without a primary key, and a serial column whose sequence is
// nearly used up.
docgen::Model shop()
{
    docgen::Model model;
    model.database = QStringLiteral("shopdb");
    model.serverVersion = QStringLiteral("14.10");
    model.schemas.push_back({QStringLiteral("shop"), QStringLiteral("owner"), {}});

    docgen::Relation orders;
    orders.oid = 100;
    orders.schema = QStringLiteral("shop");
    orders.name = QStringLiteral("orders");
    orders.comment = QStringLiteral("Orders.");
    orders.columns = {column("id", "integer"), column("note", "text")};
    orders.columns[0].defaultValue = QStringLiteral("nextval('shop.orders_id_seq'::regclass)");
    orders.options = QStringLiteral("fillfactor=90, autovacuum_enabled=false");
    model.relations.push_back(orders);

    docgen::Sequence sequence;
    sequence.schema = QStringLiteral("shop");
    sequence.name = QStringLiteral("orders_id_seq");
    sequence.type = QStringLiteral("integer");
    sequence.ownedBy = QStringLiteral("shop.orders.id");
    model.sequences.push_back(sequence);
    return model;
}

Health health()
{
    Health h;
    h.versionNumber = 140010;
    h.version = QStringLiteral("14.10");
    h.today = QDate(2026, 10, 23);
    h.user = QStringLiteral("reader");
    h.monitor = false;
    h.connections = 85;
    h.maxConnections = 100;
    h.reservedConnections = 3;
    h.settings
        = {{QStringLiteral("fsync"), QStringLiteral("off"), {}, QStringLiteral("default"), false},
           {QStringLiteral("shared_buffers"), QStringLiteral("16384"), QStringLiteral("8kB"),
            QStringLiteral("default"), true}};
    h.database.name = QStringLiteral("shopdb");
    h.database.statsAgeSeconds = 30 * 86400;
    h.database.blocksHit = 80'000;
    h.database.blocksRead = 20'000;
    h.database.checksumFailures = 2;
    h.database.collationVersion = QStringLiteral("2.31");
    h.database.actualCollationVersion = QStringLiteral("2.36");
    h.databases = {{QStringLiteral("shopdb"), 1'600'000'000, 10}};
    h.sessions = {{4242, QStringLiteral("app"), QStringLiteral("shopdb"), QStringLiteral("web"),
                   QStringLiteral("idle in transaction"), 600, 590, QStringLiteral("UPDATE x")},
                  {4243,
                   QStringLiteral("app"),
                   QStringLiteral("shopdb"),
                   {},
                   QStringLiteral("active"),
                   30,
                   30,
                   QStringLiteral("SELECT 1")}};
    h.prepared
        = {{QStringLiteral("it's old"), QStringLiteral("app"), QStringLiteral("shopdb"), 3600}};
    h.slots = {{QStringLiteral("gone"), QStringLiteral("logical"), QStringLiteral("shopdb"), false,
                20LL * 1024 * 1024 * 1024, QStringLiteral("extended")},
               {QStringLiteral("fine"),
                QStringLiteral("physical"),
                {},
                true,
                1024,
                QStringLiteral("reserved")}};
    TableHealth orders;
    orders.oid = 100;
    orders.schema = QStringLiteral("shop");
    orders.name = QStringLiteral("orders");
    orders.seqScans = 500;
    orders.seqRowsRead = 500 * 40'000;
    orders.indexScans = 10;
    orders.liveRows = 40'000;
    orders.deadRows = 20'000;
    orders.xidAge = 100;
    h.tables = {orders};
    h.indexes = {{QStringLiteral("shop"), QStringLiteral("orders"), QStringLiteral("orders_note"),
                  0, 8 * 1024 * 1024, false},
                 {QStringLiteral("shop"), QStringLiteral("orders"), QStringLiteral("orders_pkey"),
                  0, 8 * 1024 * 1024, true}};
    h.sequences = {{QStringLiteral("shop"), QStringLiteral("orders_id_seq"), 2'000'000'000, 1,
                    std::numeric_limits<qint64>::max(), 1, false}};
    h.extensionUpdates
        = {{QStringLiteral("pg_trgm"), QStringLiteral("1.5"), QStringLiteral("1.6")}};
    h.publicCreateSchemas = {QStringLiteral("shop")};
    h.unavailable = {{Part::Routines, QStringLiteral("permission denied")}};
    return h;
}

const Topic *topic(const std::vector<Topic> &topics, const char *id)
{
    const auto it = std::ranges::find(topics, QString::fromUtf8(id), &Topic::id);
    return it == topics.end() ? nullptr : &*it;
}

const Issue *issue(const std::vector<Topic> &topics, const char *id)
{
    const Topic *t = topic(topics, id);
    return t && !t->issues.empty() ? &t->issues.front() : nullptr;
}

QString read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

class TestReport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void settingDisplay()
    {
        QCOMPARE((Setting {QStringLiteral("shared_buffers"),
                           QStringLiteral("16384"),
                           QStringLiteral("8kB"),
                           {},
                           false})
                     .display(),
                 QStringLiteral("128MB"));
        QCOMPARE((Setting {QStringLiteral("work_mem"),
                           QStringLiteral("4096"),
                           QStringLiteral("kB"),
                           {},
                           false})
                     .display(),
                 QStringLiteral("4MB"));
        QCOMPARE(
            (Setting {QStringLiteral("x"), QStringLiteral("1500"), QStringLiteral("kB"), {}, false})
                .display(),
            QStringLiteral("1500kB"));
        QCOMPARE(
            (Setting {QStringLiteral("t"), QStringLiteral("200"), QStringLiteral("ms"), {}, false})
                .display(),
            QStringLiteral("200ms"));
        QCOMPARE((Setting {QStringLiteral("fsync"), QStringLiteral("on"), {}, {}, false}).display(),
                 QStringLiteral("on"));
        QCOMPARE(
            (Setting {QStringLiteral("c"), QStringLiteral("-1"), QStringLiteral("8kB"), {}, false})
                .display(),
            QStringLiteral("-1"));
    }

    void endOfLifeDates()
    {
        QCOMPARE(endOfLife(140010), QDate(2026, 11, 12));
        QCOMPARE(endOfLife(180001), QDate(2030, 11, 14));
        QVERIFY(!endOfLife(990000).isValid());
        QVERIFY(!endOfLife(0).isValid());
    }

    // The queries follow the server's version.
    void queries()
    {
        const auto sql = [](int version, Part part, const QString &plpgsqlCheck = {}) {
            for (const HealthQuery &q : healthQueries(version, {}, plpgsqlCheck)) {
                if (q.part == part)
                    return QString::fromUtf8(q.sql);
            }
            return QString();
        };
        QVERIFY(sql(180000, Part::Database).contains(QLatin1String("datcollversion")));
        QVERIFY(!sql(140000, Part::Database).contains(QLatin1String("datcollversion")));
        QVERIFY(sql(180000, Part::Server).contains(QLatin1String("'reserved_connections'")));
        QVERIFY(!sql(150000, Part::Server).contains(QLatin1String("'reserved_connections'")));
        QVERIFY(sql(130000, Part::Slots).contains(QLatin1String("wal_status")));
        QVERIFY(sql(180000, Part::Routines).isEmpty());
        QVERIFY(sql(180000, Part::Routines, QStringLiteral("My Ext"))
                    .contains(QLatin1String("\"My Ext\".plpgsql_check_function_tb")));
        const docgen::SchemaFilter filter {{QStringLiteral("shop")}, {}};
        for (const HealthQuery &q : healthQueries(180000, filter)) {
            if (q.part == Part::Tables || q.part == Part::Indexes || q.part == Part::Sequences)
                QVERIFY(q.sql.contains("LIKE ANY"));
        }
    }

    void schemaOnly()
    {
        const std::vector<Topic> topics = analyse(shop(), nullptr);
        // Every docgen check is a topic, graded; nothing needing statistics.
        QVERIFY(topic(topics, "tables-without-a-primary-key"));
        QVERIFY(topic(topics, "duplicate-indexes"));
        QVERIFY(!topic(topics, "unused-indexes"));
        QVERIFY(!topic(topics, "autovacuum-off"));
        const Issue *noKey = issue(topics, "tables-without-a-primary-key");
        QVERIFY(noKey);
        QCOMPARE(noKey->severity, Severity::Warning);
        QCOMPARE(noKey->object, QStringLiteral("shop.orders"));
        QCOMPARE(topic(topics, "tables-without-a-primary-key")->section, Section::Schema);
        QCOMPARE(topic(topics, "duplicate-constraints")->section, Section::Duplicates);
        QCOMPARE(topic(topics, "disabled-triggers")->section, Section::Dependencies);
        // In the order of the sections.
        for (std::size_t i = 1; i < topics.size(); ++i)
            QVERIFY(int(topics[i - 1].section) <= int(topics[i].section));
        // Foreign keys without an index last among the indexes, after those
        // the statistics add.
        const Health h = health();
        const std::vector<Topic> all = analyse(shop(), &h);
        std::vector<QString> indexes;
        for (const Topic &t : all) {
            if (t.section == Section::Indexes)
                indexes.push_back(t.id);
        }
        QCOMPARE(indexes.back(), QStringLiteral("foreign-keys-without-an-index"));
        QVERIFY(std::ranges::count(indexes, QStringLiteral("unused-indexes")) == 1);
    }

    void healthTopics()
    {
        const Health h = health();
        const std::vector<Topic> topics = analyse(shop(), &h);

        const Issue *version = issue(topics, "version");
        QVERIFY(version);
        QCOMPARE(version->severity, Severity::Warning); // Weeks before its end.

        const Issue *unused = issue(topics, "unused-indexes");
        QVERIFY(unused);
        QCOMPARE(unused->object, QStringLiteral("shop.orders orders_note"));
        QCOMPARE(unused->fix, QStringLiteral("DROP INDEX CONCURRENTLY shop.orders_note;"));
        QCOMPARE(topic(topics, "unused-indexes")->issues.size(), std::size_t(1));

        QVERIFY(issue(topics, "sequential-scans"));
        QCOMPARE(issue(topics, "collation-version")->severity, Severity::Critical);
        QCOMPARE(issue(topics, "extension-updates")->fix,
                 QStringLiteral("ALTER EXTENSION pg_trgm UPDATE;"));
        QVERIFY(topic(topics, "routine-errors")
                    ->unavailable.contains(QLatin1String("permission denied")));

        const Topic *settings = topic(topics, "settings");
        QCOMPARE(settings->issues.front().object, QStringLiteral("fsync = off"));
        QCOMPARE(settings->issues.front().severity, Severity::Critical);
        QVERIFY(std::ranges::any_of(settings->issues, [](const Issue &i) {
            return i.object == QLatin1String("shared_buffers") && i.severity == Severity::Warning;
        }));

        // 85 of 97 is over 80 %.
        QCOMPARE(issue(topics, "connections")->severity, Severity::Warning);

        // The idle one, not the short one.
        const Topic *transactions = topic(topics, "transactions");
        QCOMPARE(transactions->issues.size(), std::size_t(1));
        QCOMPARE(transactions->issues.front().fix,
                 QStringLiteral("SELECT pg_terminate_backend(4242);"));

        QCOMPARE(issue(topics, "prepared-transactions")->fix,
                 QStringLiteral("ROLLBACK PREPARED 'it''s old';"));

        const Topic *slots = topic(topics, "replication-slots");
        QCOMPARE(slots->issues.size(), std::size_t(1));
        QCOMPARE(slots->issues.front().severity, Severity::Critical);

        const Issue *wraparound = issue(topics, "wraparound");
        QCOMPARE(wraparound->severity, Severity::Critical);
        QCOMPARE(wraparound->fix, QStringLiteral("VACUUM (FREEZE);"));

        QCOMPARE(issue(topics, "dead-rows")->fix, QStringLiteral("VACUUM (ANALYZE) shop.orders;"));
        QCOMPARE(issue(topics, "autovacuum-off")->fix,
                 QStringLiteral("ALTER TABLE shop.orders RESET (autovacuum_enabled);"));
        QCOMPARE(issue(topics, "planner-statistics")->severity, Severity::Warning);

        // Limited by the integer column, not the bigint maximum.
        const Issue *sequence = issue(topics, "sequences");
        QVERIFY(sequence);
        QCOMPARE(sequence->severity, Severity::Critical);
        QVERIFY(sequence->fix.contains(QLatin1String("ALTER COLUMN id TYPE bigint")));
        QVERIFY(
            sequence->fix.contains(QLatin1String("ALTER SEQUENCE shop.orders_id_seq AS bigint")));

        QCOMPARE(issue(topics, "cache-hit")->severity, Severity::Warning);
        QVERIFY(
            std::ranges::any_of(topic(topics, "database-statistics")->issues,
                                [](const Issue &i) { return i.severity == Severity::Critical; }));
        QCOMPARE(issue(topics, "public-create")->fix,
                 QStringLiteral("REVOKE CREATE ON SCHEMA shop FROM PUBLIC;"));

        const Topic *visibility = topic(topics, "visibility");
        QCOMPARE(visibility->issues.size(), std::size_t(2)); // Not pg_monitor, a part missing.
        QCOMPARE(visibility->issues.front().fix, QStringLiteral("GRANT pg_monitor TO reader;"));

        QCOMPARE(worst(topics), Severity::Critical);
        QVERIFY(count(topics, Section::Health, Severity::Critical) >= 5);
        QCOMPARE(count(topics, Severity::Critical),
                 count(topics, Section::Health, Severity::Critical)
                     + count(topics, Section::Dependencies, Severity::Critical));
    }

    // Fresh statistics say nothing about which indexes are used.
    void unusedTooSoon()
    {
        Health h = health();
        h.database.statsAgeSeconds = 3600;
        const std::vector<Topic> topics = analyse(shop(), &h);
        const Topic *unused = topic(topics, "unused-indexes");
        QVERIFY(unused->issues.empty());
        QVERIFY(unused->unavailable.contains(QLatin1String("too soon")));
        Thresholds now;
        now.statisticsAgeSeconds = 0;
        QVERIFY(issue(analyse(shop(), &h, now), "unused-indexes"));
    }

    void healthy()
    {
        Health h;
        h.versionNumber = 180001;
        h.version = QStringLiteral("18.1");
        h.today = QDate(2026, 10, 23);
        h.monitor = true;
        h.maxConnections = 100;
        h.connections = 3;
        docgen::Model model;
        model.database = QStringLiteral("empty");
        const std::vector<Topic> topics = analyse(model, &h);
        QVERIFY(!worst(topics).has_value());
        QVERIFY(
            topic(topics, "routine-errors")->unavailable.contains(QLatin1String("plpgsql_check")));
    }

    void pages()
    {
        const Health h = health();
        const docgen::Model model = shop();
        const std::vector<Topic> topics = analyse(model, &h);
        ReportOptions options;
        options.generator = QStringLiteral("test");
        options.description = QStringLiteral("For the board.\n\nOnce a month.");
        const docgen::Site site = buildReport(model, &h, topics, options);

        QCOMPARE(site.title, QStringLiteral("shopdb quality report"));
        QCOMPARE(site.pages.front().id, QStringLiteral("index"));
        for (const char *id :
             {"schema", "indexes", "dependencies", "duplicates", "health", "statements"})
            QVERIFY2(site.page(QString::fromUtf8(id)), id);
        QCOMPARE(site.navigation.size(), std::size_t(7));

        // Every link goes to a page there is, and to a heading on it.
        for (const docgen::Page &page : site.pages) {
            for (const docgen::Block &block : page.blocks) {
                for (const auto &row : block.rows) {
                    for (const docgen::Inline &cell : row) {
                        for (const docgen::Span &span : cell) {
                            if (span.link.isNull())
                                continue;
                            const docgen::Page *target = site.page(span.link.page);
                            QVERIFY2(target, qPrintable(span.link.page));
                            if (span.link.anchor.isEmpty())
                                continue;
                            QVERIFY2(
                                std::ranges::any_of(target->blocks,
                                                    [&](const docgen::Block &b) {
                                                        return b.anchor == span.link.anchor;
                                                    }),
                                qPrintable(span.link.page + QLatin1Char('#') + span.link.anchor));
                        }
                    }
                }
            }
        }

        // The summary lists the critical ones first.
        const docgen::Page &home = site.pages.front();
        const auto first
            = std::ranges::find(home.blocks, QStringLiteral("first"), &docgen::Block::anchor);
        QVERIFY(first != home.blocks.end());
        const docgen::Block &list = *(first + 1);
        QCOMPARE(list.kind, docgen::Block::Kind::Table);
        QCOMPARE(list.rows.front().front().front().text, QStringLiteral("Critical"));

        // The statements, the COMMENT ON templates aside.
        const docgen::Page *statements = site.page(QStringLiteral("statements"));
        QString script;
        for (const docgen::Block &block : statements->blocks)
            script += block.code;
        QVERIFY(script.contains(QLatin1String("DROP INDEX CONCURRENTLY shop.orders_note;")));
        QVERIFY(script.contains(QLatin1String("-- Unused indexes")));
        QVERIFY(!script.contains(QLatin1String("COMMENT ON")));
    }

    void withoutHealth()
    {
        const docgen::Model model = shop();
        const docgen::Site site = buildReport(model, nullptr, analyse(model, nullptr), {});
        const docgen::Page *healthPage = site.page(QStringLiteral("health"));
        QVERIFY(healthPage);
        QVERIFY(std::ranges::none_of(healthPage->blocks, [](const docgen::Block &b) {
            return b.anchor == QLatin1String("overview");
        }));
    }

    void writes_data()
    {
        QTest::addColumn<docgen::Format>("format");
        QTest::addColumn<QString>("entry");
        QTest::newRow("html") << docgen::Format::Html << QStringLiteral("index.html");
        QTest::newRow("markdown") << docgen::Format::Markdown << QStringLiteral("index.md");
        QTest::newRow("asciidoc") << docgen::Format::AsciiDoc << QStringLiteral("index.adoc");
        QTest::newRow("antora") << docgen::Format::Antora
                                << QStringLiteral("modules/ROOT/pages/index.adoc");
        QTest::newRow("sphinx") << docgen::Format::Sphinx << QStringLiteral("index.rst");
    }

    void writes()
    {
        QFETCH(docgen::Format, format);
        QFETCH(QString, entry);
        QTemporaryDir directory;
        Options options;
        options.output.format = format;
        options.output.directory = directory.path();
        options.notices = false;
        const Generated generated = generate({shop(), health()}, options);
        QVERIFY2(generated.written.ok(), qPrintable(generated.written.error));
        QCOMPARE(generated.written.pages, 7);
        QCOMPARE(count(generated.topics, Severity::Notice), 0);
        QVERIFY(count(generated.topics, Severity::Critical) > 0);
        const QString home = read(directory.filePath(entry));
        QVERIFY(home.contains(QLatin1String("shopdb quality report")));
        QVERIFY(home.contains(QLatin1String("slonisko-quality-report")));
        QVERIFY(read(directory.filePath(docgen::pageFile(QStringLiteral("indexes"), format)))
                    .contains(QLatin1String("orders_note")));
    }
};

QTEST_MAIN(TestReport)
#include "tst_quality.moc"
