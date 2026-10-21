// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Export.h"

#include <QTest>
#include <QTextStream>

#include <cstring>

using namespace slonisko::catalog;
using slonisko::pg::Result;
using slonisko::pg::RowStore;

namespace {

// A result built in memory; nullptr values are NULLs.
Result makeResult(std::initializer_list<std::pair<const char *, Oid>> columns,
                  std::initializer_list<std::initializer_list<const char *>> rows)
{
    PGresult *r = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    std::vector<PGresAttDesc> attrs;
    int attnum = 1;
    for (const auto &[name, type] : columns)
        attrs.push_back({const_cast<char *>(name), 0, attnum++, 0, type, -1, -1});
    PQsetResultAttrs(r, int(attrs.size()), attrs.data());
    int row = 0;
    for (const auto &values : rows) {
        int c = 0;
        for (const char *v : values)
            PQsetvalue(r, row, c++, const_cast<char *>(v), v ? int(std::strlen(v)) : -1);
        ++row;
    }
    return Result(r);
}

RowStore store()
{
    RowStore rows;
    rows.append(makeResult({{"id", 23}, {"name", 25}, {"note", 25}},
                           {{"1", "Alice", "plain"},
                            {"2", "Bo, \"the\" Builder", nullptr},
                            {"42", "line\nbreak", "it's"}}));
    return rows;
}

ExportOptions as(ExportFormat format)
{
    ExportOptions options;
    options.format = format;
    return options;
}

} // namespace

class TestExport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void csv()
    {
        ExportOptions options;
        options.format = ExportFormat::Csv;
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("id,name,note\n"
                                "1,Alice,plain\n"
                                "2,\"Bo, \"\"the\"\" Builder\",\n"
                                "42,\"line\nbreak\",it's\n"));

        options.header = false;
        options.nullText = QStringLiteral("\\N");
        options.columns = {0, 2};
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("1,plain\n"
                                "2,\\N\n"
                                "42,it's\n"));
    }

    void tsv()
    {
        ExportOptions options;
        options.format = ExportFormat::Tsv;
        options.maxRows = 1;
        QCOMPARE(exportRows(store(), options), QStringLiteral("id\tname\tnote\n1\tAlice\tplain\n"));
    }

    void textTable()
    {
        ExportOptions options;
        options.format = ExportFormat::Text;
        // Numbers right, text left, NULLs marked, line breaks kept on one line.
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("id | name              | note\n"
                                "---+-------------------+-------\n"
                                " 1 | Alice             | plain\n"
                                " 2 | Bo, \"the\" Builder | [NULL]\n"
                                "42 | line\\nbreak       | it's\n"));
    }

    void textTableCutsWideCells()
    {
        ExportOptions options;
        options.format = ExportFormat::Text;
        options.maxCellWidth = 6;
        options.maxRows = 2;
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("id | name   | note\n"
                                "---+--------+-------\n"
                                " 1 | Alice  | plain\n"
                                " 2 | Bo, \"… | [NULL]\n"
                                "… 1 more row\n"));
    }

    void sqlInserts()
    {
        ExportOptions options;
        options.format = ExportFormat::SqlInsert;
        options.table = QStringLiteral("public.people");
        options.maxRows = 2;
        // Numbers unquoted, quotes doubled, NULL as NULL.
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("INSERT INTO public.people (id, name, note) VALUES "
                                "(1, 'Alice', 'plain');\n"
                                "INSERT INTO public.people (id, name, note) VALUES "
                                "(2, 'Bo, \"the\" Builder', NULL);\n"));
    }

    void sqlBulkInserts()
    {
        ExportOptions options;
        options.format = ExportFormat::SqlBulkInsert;
        options.table = QStringLiteral("people");
        options.bulkRows = 2;
        options.columns = {0, 1};
        QCOMPARE(exportRows(store(), options),
                 QStringLiteral("INSERT INTO people (id, name) VALUES\n"
                                "    (1, 'Alice'),\n"
                                "    (2, 'Bo, \"the\" Builder');\n"
                                "INSERT INTO people (id, name) VALUES\n"
                                "    (42, 'line\nbreak');\n"));
    }

    void quotesOddIdentifiers()
    {
        RowStore rows;
        rows.append(makeResult({{"Mixed Case", 25}, {"ok", 16}}, {{"x", "t"}, {"y", "f"}}));
        ExportOptions options;
        options.format = ExportFormat::SqlInsert;
        QCOMPARE(
            exportRows(rows, options),
            QStringLiteral("INSERT INTO table_name (\"Mixed Case\", ok) VALUES ('x', true);\n"
                           "INSERT INTO table_name (\"Mixed Case\", ok) VALUES ('y', false);\n"));
    }

    void emptyResult()
    {
        RowStore rows;
        ExportOptions options;
        QVERIFY(exportRows(rows, options).isEmpty());
        rows.append(makeResult({{"id", 23}}, {}));
        options.format = ExportFormat::Csv;
        QCOMPARE(exportRows(rows, options), QStringLiteral("id\n"));
    }

    void json()
    {
        RowStore rows;
        rows.append(makeResult({{"id", 23}, {"doc", 3802}, {"ok", 16}, {"x", 701}},
                               {{"1", "{\"a\": [1]}", "t", "NaN"}, {"2", nullptr, "f", "-1.5"}}));
        ExportOptions options;
        options.format = ExportFormat::Json;
        // Numbers and booleans as such, json as it is; NaN is no JSON number.
        QCOMPARE(
            exportRows(rows, options),
            QStringLiteral("[\n"
                           "  {\"id\": 1, \"doc\": {\"a\": [1]}, \"ok\": true, \"x\": \"NaN\"},\n"
                           "  {\"id\": 2, \"doc\": null, \"ok\": false, \"x\": -1.5}\n"
                           "]\n"));
        ExportOptions lines = as(ExportFormat::JsonLines);
        lines.maxRows = 1;
        QCOMPARE(exportRows(store(), lines),
                 QStringLiteral("{\"id\": 1, \"name\": \"Alice\", \"note\": \"plain\"}\n"));
        const QString escaped = exportRows(store(), as(ExportFormat::JsonLines));
        QVERIFY(escaped.contains(QLatin1String("\"Bo, \\\"the\\\" Builder\"")));
        QVERIFY(escaped.contains(QLatin1String("\"line\\nbreak\"")));

        RowStore empty;
        empty.append(makeResult({{"id", 23}}, {}));
        QCOMPARE(exportRows(empty, as(ExportFormat::Json)), QStringLiteral("[]\n"));
    }

    void markdown()
    {
        RowStore rows;
        rows.append(makeResult({{"id", 23}, {"a|b", 25}}, {{"1", "x|y\nz"}, {"2", nullptr}}));
        ExportOptions options;
        options.format = ExportFormat::Markdown;
        options.nullText = QStringLiteral("NULL");
        QCOMPARE(exportRows(rows, options),
                 QStringLiteral("| id | a\\|b |\n"
                                "|---:|---|\n"
                                "| 1 | x\\|y<br>z |\n"
                                "| 2 | NULL |\n"));
    }

    void html()
    {
        RowStore rows;
        rows.append(makeResult({{"id", 23}, {"<b>", 25}}, {{"1", "a & b"}}));
        ExportOptions options;
        options.format = ExportFormat::Html;
        const QString out = exportRows(rows, options);
        QVERIFY(out.startsWith(QLatin1String("<!DOCTYPE html>")));
        QVERIFY(out.contains(QLatin1String("<meta charset=\"utf-8\">")));
        QVERIFY(out.contains(QLatin1String("<tr><th>id</th><th>&lt;b&gt;</th></tr>")));
        QVERIFY(
            out.contains(QLatin1String("<tr><td class=\"number\">1</td><td>a &amp; b</td></tr>")));
        QVERIFY(out.endsWith(QLatin1String("</table>\n</body>\n</html>\n")));
    }

    void xml()
    {
        RowStore rows;
        rows.append(makeResult({{"id", 23}, {"note", 25}}, {{"1", "a<b\x01"}, {"2", nullptr}}));
        QCOMPARE(exportRows(rows, as(ExportFormat::Xml)),
                 QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<rows>\n"
                                "  <row>\n"
                                "    <column name=\"id\">1</column>\n"
                                "    <column name=\"note\">a&lt;b\uFFFD</column>\n"
                                "  </row>\n"
                                "  <row>\n"
                                "    <column name=\"id\">2</column>\n"
                                "    <column name=\"note\" null=\"true\"/>\n"
                                "  </row>\n"
                                "</rows>\n"));
    }

    void sqlCopy()
    {
        RowStore rows;
        rows.append(makeResult({{"id", 23}, {"Note", 25}}, {{"1", "tab\there\\"}, {"2", nullptr}}));
        ExportOptions options;
        options.format = ExportFormat::SqlCopy;
        options.table = QStringLiteral("public.t");
        // What psql and the editor run as it is.
        QCOMPARE(exportRows(rows, options),
                 QStringLiteral("COPY public.t (id, \"Note\") FROM stdin;\n"
                                "1\ttab\\there\\\\\n"
                                "2\t\\N\n"
                                "\\.\n"));
    }

    // A query's rows arrive in parts; the document around them is one.
    void writesPartsAsOne()
    {
        RowStore first;
        first.append(makeResult({{"id", 23}}, {{"1"}, {"2"}}));
        RowStore second;
        second.append(makeResult({{"id", 23}}, {{"3"}}));
        auto parts = [&](const ExportOptions &options) {
            QString out;
            QTextStream stream(&out);
            RowWriter writer(options, stream);
            writer.write(first);
            writer.write(second);
            writer.finish();
            stream.flush();
            return out;
        };
        QCOMPARE(parts(as(ExportFormat::Csv)), QStringLiteral("id\n1\n2\n3\n"));
        QCOMPARE(parts(as(ExportFormat::Json)),
                 QStringLiteral("[\n  {\"id\": 1},\n  {\"id\": 2},\n  {\"id\": 3}\n]\n"));
        ExportOptions bulk = as(ExportFormat::SqlBulkInsert);
        bulk.table = QStringLiteral("t");
        bulk.bulkRows = 2;
        QCOMPARE(parts(bulk),
                 QStringLiteral("INSERT INTO t (id) VALUES\n    (1),\n    (2);\n"
                                "INSERT INTO t (id) VALUES\n    (3);\n"));
        QCOMPARE(parts(as(ExportFormat::Text)), QStringLiteral("id\n--\n 1\n 2\n 3\n"));
    }

    void namesAndSuffixes()
    {
        QCOMPARE(fileSuffix(ExportFormat::Csv), QStringLiteral("csv"));
        QCOMPARE(fileSuffix(ExportFormat::SqlBulkInsert), QStringLiteral("sql"));
        QVERIFY(fileFilter(ExportFormat::Text).contains(QLatin1String("*.txt")));
        QVERIFY(!formatName(ExportFormat::SqlInsert).isEmpty());
        QCOMPARE(fileSuffix(ExportFormat::JsonLines), QStringLiteral("jsonl"));
        QCOMPARE(fileSuffix(ExportFormat::Markdown), QStringLiteral("md"));
        QCOMPARE(fileSuffix(ExportFormat::SqlCopy), QStringLiteral("sql"));
    }
};

QTEST_MAIN(TestExport)
#include "tst_export.moc"
