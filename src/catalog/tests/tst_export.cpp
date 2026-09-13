// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Export.h"

#include <QTest>

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

    void namesAndSuffixes()
    {
        QCOMPARE(fileSuffix(ExportFormat::Csv), QStringLiteral("csv"));
        QCOMPARE(fileSuffix(ExportFormat::SqlBulkInsert), QStringLiteral("sql"));
        QVERIFY(fileFilter(ExportFormat::Text).contains(QLatin1String("*.txt")));
        QVERIFY(!formatName(ExportFormat::SqlInsert).isEmpty());
    }
};

QTEST_MAIN(TestExport)
#include "tst_export.moc"
