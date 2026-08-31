// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Editing.h"

#include <QTest>

#include <cstring>

using namespace slonisko::catalog;
using slonisko::pg::Result;
using slonisko::pg::RowStore;
using Kind = RowEdit::Kind;

namespace {

// A result built in memory whose columns come from tables: {name, table, attnum}.
Result makeResult(std::initializer_list<std::tuple<const char *, Oid, int>> columns,
                  std::initializer_list<std::initializer_list<const char *>> rows = {})
{
    PGresult *r = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    std::vector<PGresAttDesc> attrs;
    for (const auto &[name, table, attnum] : columns)
        attrs.push_back({const_cast<char *>(name), table, attnum, 0, 25, -1, -1});
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

// What editTargetQuery() returns for a table.
std::vector<Result>
description(const char *kind, const char *key,
            std::initializer_list<std::tuple<const char *, const char *, const char *>> attributes)
{
    std::vector<Result> out;
    out.push_back(
        makeResult({{"nspname", 0, 0}, {"relname", 0, 0}, {"relkind", 0, 0}, {"key", 0, 0}},
                   {{"public", "My Table", kind, key}}));
    PGresult *r = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    PGresAttDesc attrs[3] = {{const_cast<char *>("attnum"), 0, 0, 0, 23, -1, -1},
                             {const_cast<char *>("attname"), 0, 0, 0, 25, -1, -1},
                             {const_cast<char *>("generated"), 0, 0, 0, 16, -1, -1}};
    PQsetResultAttrs(r, 3, attrs);
    int row = 0;
    for (const auto &[attnum, name, generated] : attributes) {
        PQsetvalue(r, row, 0, const_cast<char *>(attnum), int(std::strlen(attnum)));
        PQsetvalue(r, row, 1, const_cast<char *>(name), int(std::strlen(name)));
        PQsetvalue(r, row, 2, const_cast<char *>(generated), 1);
        ++row;
    }
    out.push_back(Result(r));
    return out;
}

RowStore store(const Result &r)
{
    RowStore s;
    s.append(r);
    return s;
}

} // namespace

class TestEditing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void target()
    {
        // SELECT name AS n, id, lower(name) FROM "My Table"
        const RowStore rows = store(makeResult({{"n", 42, 2}, {"id", 42, 1}, {"lower", 0, 0}}));
        QCOMPARE(sourceTable(rows), 42u);
        const EditTarget t
            = editTarget(rows, description("r", "1", {{"1", "id", "f"}, {"2", "name", "f"}}));
        QVERIFY2(t.editable(), qPrintable(t.reason));
        QCOMPARE(t.table, QStringLiteral("My Table"));
        QCOMPARE(t.columns,
                 (std::vector<QString> {QStringLiteral("name"), QStringLiteral("id"), QString()}));
        QCOMPARE(t.key, std::vector<int> {1});
        QVERIFY(t.writable(0));
        QVERIFY(!t.writable(2)); // Computed.
    }

    void readOnlyReasons()
    {
        QString reason;
        QCOMPARE(sourceTable(store(makeResult({{"x", 0, 0}})), &reason), 0u);
        QVERIFY(reason.contains(QLatin1String("not come from a table")));
        QCOMPARE(sourceTable(store(makeResult({{"a", 1, 1}, {"b", 2, 1}})), &reason), 0u);
        QVERIFY(reason.contains(QLatin1String("several tables")));

        const RowStore rows = store(makeResult({{"name", 42, 2}}));
        EditTarget t
            = editTarget(rows, description("r", "", {{"1", "id", "f"}, {"2", "name", "f"}}));
        QVERIFY(!t.editable());
        QVERIFY(t.reason.contains(QLatin1String("no primary key")));
        t = editTarget(rows, description("r", "1", {{"1", "id", "f"}, {"2", "name", "f"}}));
        QVERIFY(t.reason.contains(QLatin1String("lacks the primary key column(s) id")));
        t = editTarget(rows, description("v", "", {{"2", "name", "f"}}));
        QVERIFY(t.reason.contains(QLatin1String("not a table")));
    }

    void generatedColumnsAreReadOnly()
    {
        const RowStore rows = store(makeResult({{"id", 7, 1}, {"total", 7, 2}}));
        const EditTarget t
            = editTarget(rows, description("r", "1", {{"1", "id", "t"}, {"2", "total", "t"}}));
        QVERIFY(t.editable());
        QVERIFY(!t.writable(0));
        QVERIFY(!t.writable(1));
    }

    void statements()
    {
        EditTarget t;
        t.schema = QStringLiteral("public");
        t.table = QStringLiteral("My Table");
        t.columns = {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("order")};
        t.readOnly = {false, false, false};
        t.key = {0};

        const std::vector<RowEdit> edits {
            {Kind::Update, {QByteArray("1")}, {{1, QByteArray("it's")}, {2, std::nullopt}}},
            {Kind::Delete, {QByteArray("2")}, {}},
            {Kind::Insert, {}, {{0, QByteArray("3")}, {1, QByteArray("new")}}},
            {Kind::Insert, {}, {}},
        };
        QCOMPARE(
            dmlStatements(t, edits),
            (QByteArrayList {
                "UPDATE public.\"My Table\" SET name = 'it''s', \"order\" = NULL WHERE id = '1'",
                "DELETE FROM public.\"My Table\" WHERE id = '2'",
                "INSERT INTO public.\"My Table\" (id, name) VALUES ('3', 'new')",
                "INSERT INTO public.\"My Table\" DEFAULT VALUES",
            }));
    }

    void compositeKey()
    {
        EditTarget t;
        t.schema = QStringLiteral("s");
        t.table = QStringLiteral("t");
        t.columns = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("v")};
        t.readOnly = {false, false, false};
        t.key = {0, 1};
        QCOMPARE(
            dmlStatements(
                t, {{Kind::Update, {QByteArray("1"), QByteArray("x")}, {{2, QByteArray("9")}}}}),
            QByteArrayList {"UPDATE s.t SET v = '9' WHERE a = '1' AND b = 'x'"});
    }

    void oneRow()
    {
        QVERIFY(changedOneRow("UPDATE 1"));
        QVERIFY(changedOneRow("INSERT 0 1"));
        QVERIFY(changedOneRow("DELETE 1"));
        QVERIFY(!changedOneRow("UPDATE 0"));
        QVERIFY(!changedOneRow("UPDATE 2"));
    }
};

QTEST_GUILESS_MAIN(TestEditing)
#include "tst_editing.moc"
