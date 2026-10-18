// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Generator.h"
#include "docgen/Pages.h"
#include "docgen/Writer.h"

#include <QDirIterator>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko::docgen;

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

// A small shop with a little of everything.
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

QStringList filesUnder(const QString &directory)
{
    QStringList files;
    QDirIterator it(directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << QDir(directory).relativeFilePath(it.next());
    files.sort();
    return files;
}

QString read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

class TestDocgen : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void quoting()
    {
        QCOMPARE(quoteIdentifier(QStringLiteral("orders")), QStringLiteral("orders"));
        QCOMPARE(quoteIdentifier(QStringLiteral("Big Orders")), QStringLiteral("\"Big Orders\""));
        QCOMPARE(quoteIdentifier(QStringLiteral("select")), QStringLiteral("\"select\""));
        QCOMPARE(quoteIdentifier(QStringLiteral("a\"b")), QStringLiteral("\"a\"\"b\""));
        QCOMPARE(quoteQualified(QStringLiteral("shop"), QStringLiteral("Big Orders")),
                 QStringLiteral("shop.\"Big Orders\""));
    }

    void pages()
    {
        const Model model = shop();
        PageOptions options;
        options.generator = QStringLiteral("test");
        const Site site = buildSite(model, options);

        QCOMPARE(site.title, QStringLiteral("shopdb"));
        QCOMPARE(site.pages.front().id, QStringLiteral("index"));
        for (const char *id :
             {"shop/index", "shop/tables/customer", "shop/tables/orders", "shop/views/Big_Orders",
              "shop/routines/touch", "shop/types/status", "shop/sequences", "names"})
            QVERIFY2(site.page(QString::fromUtf8(id)), id);
        // Data and data would be one file on macOS or Windows.
        QVERIFY(site.page(QStringLiteral("shop/tables/Data")));
        QVERIFY(site.page(QStringLiteral("shop/tables/data-2")));

        const Page *orders = site.page(QStringLiteral("shop/tables/orders"));
        QCOMPARE(orders->title, QStringLiteral("shop.orders"));
        QCOMPARE(orders->kind, QStringLiteral("table"));
        QCOMPARE(orders->trail.size(), std::size_t(2));
        const auto has = [](const Page *page, const char *anchor) {
            return std::ranges::any_of(
                page->blocks, [&](const Block &b) { return b.anchor == QLatin1String(anchor); });
        };
        QVERIFY(has(orders, "columns"));
        QVERIFY(has(orders, "foreign-keys"));
        QVERIFY(has(orders, "indexes"));
        QVERIFY(has(orders, "triggers"));
        QVERIFY(has(orders, "used-by"));
        QVERIFY(has(orders, "diagram"));
        QVERIFY(has(orders, "definition"));
        QVERIFY(has(site.page(QStringLiteral("shop/tables/customer")), "referenced-by"));
        QVERIFY(has(site.page(QStringLiteral("shop/routines/touch")), "triggers"));

        PageOptions bare = options;
        bare.diagrams = false;
        bare.definitions = false;
        bare.symbolIndex = false;
        const Site small = buildSite(model, bare);
        QVERIFY(!small.page(QStringLiteral("names")));
        QVERIFY(!has(small.page(QStringLiteral("shop/tables/orders")), "diagram"));
        QVERIFY(!has(small.page(QStringLiteral("shop/tables/orders")), "definition"));
    }

    void graphs()
    {
        const Model model = shop();
        const auto graph = tableGraph(model, 200);
        QCOMPARE(graph.focus, 200u);
        QCOMPARE(graph.nodes.size(), std::size_t(2)); // orders and customer
        QCOMPARE(graph.edges.size(), std::size_t(1));
        QVERIFY(graph.edges.front().mandatory);
        QCOMPARE(graph.nodes.front().otherColumns, 1); // created

        const auto schema = schemaGraph(model, QStringLiteral("shop"));
        QCOMPARE(schema.nodes.size(), std::size_t(5)); // Not the view.
    }

    void definition()
    {
        const Model model = shop();
        const QString sql = tableDefinition(model.relations[4]);
        QVERIFY2(sql.startsWith(QLatin1String("CREATE TABLE shop.orders (\n    id bigint DEFAULT "
                                              "nextval('shop.orders_id_seq'::regclass) NOT NULL,")),
                 qPrintable(sql));
        QVERIFY(sql.contains(QLatin1String("    CONSTRAINT orders_pkey PRIMARY KEY (id)")));
        // Its index comes with the primary key; the other one is its own.
        QVERIFY(!sql.contains(QLatin1String("CREATE INDEX orders_pkey")));
        QVERIFY(sql.contains(QLatin1String("CREATE INDEX orders_id_idx")));
        QVERIFY(sql.contains(QLatin1String("CREATE TRIGGER orders_touch")));
        QVERIFY(sql.contains(QLatin1String("COMMENT ON TABLE shop.orders IS 'Orders | with")));

        Relation partition = model.relations[3];
        partition.parentSchema = QStringLiteral("shop");
        partition.parentName = QStringLiteral("Big Log");
        partition.partitionBound = QStringLiteral("FOR VALUES IN (1)");
        QVERIFY(tableDefinition(partition).startsWith(QLatin1String(
            "CREATE TABLE shop.log PARTITION OF shop.\"Big Log\"\n    FOR VALUES IN (1);")));
    }

    void links()
    {
        const Target column {QStringLiteral("shop/tables/customer"), QStringLiteral("col-id")};
        QCOMPARE(linkTo(QStringLiteral("shop/tables/orders"), column, Format::Html),
                 QStringLiteral("customer.html#col-id"));
        QCOMPARE(linkTo(QStringLiteral("shop/views/v"), column, Format::Markdown),
                 QStringLiteral("../tables/customer.md#col-id"));
        QCOMPARE(linkTo(QStringLiteral("shop/tables/orders"), {QStringLiteral("index"), {}},
                        Format::AsciiDoc),
                 QStringLiteral("../../index.adoc"));
        QCOMPARE(linkTo(QStringLiteral("index"), column, Format::Html),
                 QStringLiteral("shop/tables/customer.html#col-id"));
        QCOMPARE(linkTo(QStringLiteral("shop/tables/customer"), column, Format::Html),
                 QStringLiteral("#col-id"));
        // Antora names pages from the module's root, wherever the link is.
        QCOMPARE(linkTo(QStringLiteral("shop/tables/orders"), column, Format::Antora),
                 QStringLiteral("shop/tables/customer.adoc#col-id"));
        QCOMPARE(pageFile(QStringLiteral("shop/index"), Format::Antora),
                 QStringLiteral("modules/ROOT/pages/shop/index.adoc"));
    }

    void escaping()
    {
        const Model model = shop();
        const Site site = buildSite(model, {});
        const Page *orders = site.page(QStringLiteral("shop/tables/orders"));

        const QString markdown = renderMarkdown(site, *orders);
        QVERIFY(markdown.contains(QLatin1String("Orders \\| with a pipe and \\*stars\\*.")));
        QVERIFY(markdown.contains(QLatin1String("```mermaid")));
        QVERIFY(markdown.contains(QLatin1String("<a id=\"col-customer_id\"></a>customer\\_id")));

        const QString asciidoc = renderAsciiDoc(site, *orders, Format::AsciiDoc);
        QVERIFY(asciidoc.startsWith(QLatin1String("= shop.orders\n")));
        QVERIFY(asciidoc.contains(QLatin1String("pass:c[Orders | with a pipe and *stars*.]")));
        QVERIFY(asciidoc.contains(QLatin1String("[graphviz,format=svg]")));
        QVERIFY(asciidoc.contains(QLatin1String("xref:customer.adoc#col-id[")));
        // A pipe in a cell would end it.
        const Page *schema = site.page(QStringLiteral("shop/index"));
        QVERIFY(!renderAsciiDoc(site, *schema, Format::AsciiDoc)
                     .contains(QLatin1String("| Orders | with")));

        const QString html = renderHtml(site, *orders);
        QVERIFY(html.contains(QLatin1String("<link rel=\"stylesheet\" href=\"../../style.css\">")));
        QVERIFY(html.contains(QLatin1String("Orders | with a pipe and *stars*.")));
        QVERIFY(html.contains(QLatin1String("<tr id=\"col-customer_id\">")));
        QVERIFY(html.contains(QLatin1String("<span class=\"k\">CREATE</span>")));
        // No Graphviz output given: the source, for whoever wants to draw it.
        QVERIFY(html.contains(QLatin1String("<details class=\"diagram-source\">")));
    }

    void sphinxMarkup()
    {
        const Model model = shop();
        const Site site = buildSite(model, {});
        const Page *orders = site.page(QStringLiteral("shop/tables/orders"));
        const QString rst = renderSphinx(site, *orders);
        QVERIFY2(rst.startsWith(QLatin1String("===========\nshop.orders\n===========\n")),
                 qPrintable(rst.left(80)));
        QVERIFY(rst.contains(QLatin1String("Orders \\| with a pipe and \\*stars\\*.")));
        QVERIFY(
            rst.contains(QLatin1String(".. _shop-tables-orders--columns:\n\nColumns\n-------")));
        // A row's label sits in its first cell.
        QVERIFY(rst.contains(QLatin1String(
            "   * - .. _shop-tables-orders--col-customer_id:\n\n       customer\\_id")));
        // FK → customer.id: right after the arrow, the space keeps them apart.
        QVERIFY(rst.contains(
            QString::fromUtf8("FK → :ref:`customer.id <shop-tables-customer--col-id>`")));
        QVERIFY(rst.contains(QLatin1String(":doc:`shop.customer </shop/tables/customer>`")));
        QVERIFY(rst.contains(QLatin1String(".. code-block:: postgresql")));
        QVERIFY(rst.contains(QLatin1String(".. graphviz::")));

        // The home page and each schema hold the toctree of the pages below.
        const QString home = renderSphinx(site, site.pages.front());
        QVERIFY(home.contains(QLatin1String(".. toctree::\n   :hidden:\n\n   /shop/index\n")));
        const QString schema = renderSphinx(site, *site.page(QStringLiteral("shop/index")));
        QVERIFY(schema.contains(QLatin1String("   /shop/tables/orders\n")));
        QVERIFY(schema.contains(QLatin1String("   /shop/sequences\n")));
        // Before the first section, or the sidebar files the pages under it.
        QVERIFY(schema.indexOf(QLatin1String(".. toctree::"))
                < schema.indexOf(QLatin1String("\n----")));

        QVERIFY(sphinxConfiguration(site).contains(QLatin1String("project = \"shopdb\"")));
    }

    // What Sphinx itself makes of it, warnings counting as errors.
    void sphinxBuilds()
    {
        const QString sphinx = QStandardPaths::findExecutable(QStringLiteral("sphinx-build"));
        if (sphinx.isEmpty())
            QSKIP("Sphinx is not installed");
        QTemporaryDir directory;
        Options options;
        options.output.format = Format::Sphinx;
        options.output.directory = directory.path();
        // Its graphviz extension needs dot; without it, no diagrams.
        options.pages.diagrams = !QStandardPaths::findExecutable(QStringLiteral("dot")).isEmpty();
        Model model = shop();
        model.database = QStringLiteral("shop \"db\"");
        const WriteResult result = generate(model, options);
        QVERIFY2(result.ok(), qPrintable(result.error));

        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(sphinx,
                      {QStringLiteral("-q"), QStringLiteral("-W"), QStringLiteral("--keep-going"),
                       QStringLiteral("-b"), QStringLiteral("html"), QStringLiteral("."),
                       QStringLiteral("_build")});
        QVERIFY(process.waitForFinished(120'000));
        QVERIFY2(process.exitCode() == 0, process.readAll().constData());
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("_build/shop/tables/Data.html"))));
        QVERIFY(
            QFile::exists(directory.filePath(QStringLiteral("_build/shop/tables/data-2.html"))));
        const QString orders
            = read(directory.filePath(QStringLiteral("_build/shop/tables/orders.html")));
        QVERIFY(
            orders.contains(QLatin1String("href=\"customer.html#shop-tables-customer-col-id\"")));
    }

    // Every link of every page goes to a file there is, and to an anchor on it.
    void writtenLinksResolve_data()
    {
        QTest::addColumn<int>("formatNumber");
        for (const Format format : allFormats())
            QTest::newRow(qPrintable(formatKey(format))) << int(format);
    }

    void writtenLinksResolve()
    {
        QFETCH(int, formatNumber);
        const auto format = Format(formatNumber);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Options options;
        options.pages.generator = QStringLiteral("test");
        options.output.format = format;
        options.output.directory = directory.path();
        int progress = 0;
        options.output.progress = [&progress](int done, int) { progress = done; };
        // Draw the HTML diagrams if Graphviz is there, to check its links too.
        const WriteResult result = generate(shop(), options);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(progress, result.pages);
        QVERIFY(QFile::exists(result.entryPoint));

        const QStringList files = filesUnder(directory.path());
        const QString root
            = format == Format::Antora ? QStringLiteral("modules/ROOT/pages/") : QString();
        QVERIFY(files.contains(pageFile(QStringLiteral("index"), format)));
        if (format == Format::Html)
            QVERIFY(files.contains(QStringLiteral("style.css")));
        if (format == Format::Antora) {
            QVERIFY(files.contains(QStringLiteral("antora.yml")));
            QVERIFY(files.contains(QStringLiteral("modules/ROOT/nav.adoc")));
            QVERIFY(
                read(directory.filePath(QStringLiteral("antora.yml")))
                    .startsWith(QLatin1String("name: schema\ntitle: \"shopdb\"\nversion: ~\n")));
        }

        if (format == Format::Sphinx) {
            // Documents by their names from the root, anchors by labels
            // that are one namespace for the whole project.
            QVERIFY(files.contains(QStringLiteral("conf.py")));
            QSet<QString> labels;
            const QRegularExpression labelPattern(QStringLiteral("\\.\\. _([^:\\s]+):"));
            for (const QString &file : files) {
                for (const auto &match : labelPattern.globalMatch(read(directory.filePath(file)))) {
                    const QString name = match.captured(1);
                    QVERIFY2(!labels.contains(name), qPrintable(name));
                    labels.insert(name);
                }
            }
            const QRegularExpression rolePattern(
                QStringLiteral(":(doc|ref):`(?:[^`\\\\]|\\\\.)*<([^<>]+)>`"));
            const QRegularExpression tocPattern(QStringLiteral("^   /(\\S+)$"),
                                                QRegularExpression::MultilineOption);
            int checked = 0;
            for (const QString &file : files) {
                if (!file.endsWith(QLatin1String(".rst")))
                    continue;
                const QString text = read(directory.filePath(file));
                for (const auto &match : rolePattern.globalMatch(text)) {
                    const QString target = match.captured(2);
                    if (match.captured(1) == QLatin1String("doc"))
                        QVERIFY2(files.contains(target.mid(1) + QStringLiteral(".rst")),
                                 qPrintable(QStringLiteral("%1 links to %2").arg(file, target)));
                    else
                        QVERIFY2(labels.contains(target),
                                 qPrintable(QStringLiteral("%1 links to %2").arg(file, target)));
                    ++checked;
                }
                for (const auto &match : tocPattern.globalMatch(text))
                    QVERIFY(files.contains(match.captured(1) + QStringLiteral(".rst")));
            }
            QVERIFY(checked > 50);
            return;
        }

        QRegularExpression linkPattern;
        QRegularExpression anchorPattern;
        switch (format) {
        case Format::Html:
            linkPattern.setPattern(QStringLiteral("(?:href|xlink:href)=\"([^\"]+)\""));
            anchorPattern.setPattern(QStringLiteral("id=\"([^\"]+)\""));
            break;
        case Format::Markdown:
            linkPattern.setPattern(QStringLiteral("\\]\\(([^)]+)\\)"));
            anchorPattern.setPattern(QStringLiteral("<a id=\"([^\"]+)\">"));
            break;
        default:
            linkPattern.setPattern(QStringLiteral("xref:([^\\[]+)\\[|<<([^,>]+),"));
            anchorPattern.setPattern(QStringLiteral("\\[\\[([^\\]]+)\\]\\]"));
        }

        int checked = 0;
        for (const QString &file : files) {
            if (!file.endsWith(QLatin1String(".html")) && !file.endsWith(QLatin1String(".md"))
                && !file.endsWith(QLatin1String(".adoc")))
                continue;
            const QString text = read(directory.filePath(file));
            for (const QRegularExpressionMatch &match : linkPattern.globalMatch(text)) {
                QString target = match.captured(1);
                if (target.isEmpty())
                    target = QLatin1Char('#') + match.captured(2);
                if (target.endsWith(QLatin1String(".css")))
                    continue;
                const qsizetype hash = target.indexOf(QLatin1Char('#'));
                const QString path = hash < 0 ? target : target.left(hash);
                const QString anchor = hash < 0 ? QString() : target.mid(hash + 1);
                QString resolved;
                if (path.isEmpty())
                    resolved = file;
                else if (format == Format::Antora)
                    resolved = root + path;
                else
                    resolved = QDir::cleanPath(QFileInfo(file).path() + QLatin1Char('/') + path);
                if (resolved.startsWith(QLatin1String("./")))
                    resolved = resolved.mid(2);
                QVERIFY2(files.contains(resolved),
                         qPrintable(QStringLiteral("%1 links to %2").arg(file, target)));
                if (!anchor.isEmpty()) {
                    bool found = false;
                    for (const QRegularExpressionMatch &a :
                         anchorPattern.globalMatch(read(directory.filePath(resolved))))
                        found = found || a.captured(1) == anchor;
                    QVERIFY2(found, qPrintable(QStringLiteral("%1 links to %2").arg(file, target)));
                }
                ++checked;
            }
        }
        QVERIFY(checked > 50);
    }

    void graphvizDrawsDiagrams()
    {
        const QString dot = QStandardPaths::findExecutable(QStringLiteral("dot"));
        if (dot.isEmpty())
            QSKIP("Graphviz is not installed");
        QTemporaryDir directory;
        Options options;
        options.output.directory = directory.path();
        options.output.dot = dot;
        const WriteResult result = generate(shop(), options);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join(QLatin1Char('\n'))));
        const QString orders = read(directory.filePath(QStringLiteral("shop/tables/orders.html")));
        QVERIFY(orders.contains(QLatin1String("<div class=\"diagram\">\n<svg")));
        // The other table of the diagram is a link to its page.
        QVERIFY(orders.contains(QLatin1String("href=\"customer.html\"")));
    }

    void missingStylesheet()
    {
        QTemporaryDir directory;
        Options options;
        options.pages.diagrams = false;
        options.output.directory = directory.path();
        options.output.stylesheet = directory.filePath(QStringLiteral("nothing.css"));
        const WriteResult result = generate(shop(), options);
        QVERIFY(!result.ok());
        QVERIFY(result.error.contains(QLatin1String("nothing.css")));
    }

    void commandLine()
    {
        QProcess process;
        process.start(QStringLiteral(SLONISKO_DOCGEN), {QStringLiteral("--version")});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains(SLONISKO_VERSION));

        // The format comes first, and is not optional.
        process.start(QStringLiteral(SLONISKO_DOCGEN), {QStringLiteral("pdf")});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("unknown format pdf"));
        process.start(QStringLiteral(SLONISKO_DOCGEN), {QStringLiteral("-q")});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("no format"));

        // Each format has options of its own, and only those.
        process.start(QStringLiteral(SLONISKO_DOCGEN),
                      {QStringLiteral("html"), QStringLiteral("--help")});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 0);
        const QByteArray help = process.readAllStandardOutput();
        QVERIFY(help.startsWith("Usage: slonisko-docgen html [options] [connection]"));
        QVERIFY(help.contains("--css"));
        QVERIFY(!help.contains("--antora-name"));
        process.start(
            QStringLiteral(SLONISKO_DOCGEN),
            {QStringLiteral("markdown"), QStringLiteral("--css"), QStringLiteral("a.css")});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("css"));

        // Nothing listens there: it says so and fails.
        process.start(
            QStringLiteral(SLONISKO_DOCGEN),
            {QStringLiteral("html"), QStringLiteral("host=127.0.0.1 port=1 connect_timeout=5")});
        QVERIFY(process.waitForFinished(30'000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("slonisko-docgen: "));
    }
};

QTEST_GUILESS_MAIN(TestDocgen)
#include "tst_docgen.moc"
