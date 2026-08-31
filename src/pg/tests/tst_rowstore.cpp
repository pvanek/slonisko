// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/RowStore.h"

#include <QTest>

#include <cstring>

using slonisko::pg::Result;
using slonisko::pg::RowStore;

namespace {

// A result built in memory; nullptr values are NULLs.
Result makeResult(std::initializer_list<const char *> columns,
                  std::initializer_list<std::initializer_list<const char *>> rows, Oid table = 0)
{
    PGresult *r = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
    std::vector<PGresAttDesc> attrs;
    int attnum = 1;
    for (const char *name : columns)
        attrs.push_back({const_cast<char *>(name), table, attnum++, 0, 25 /* text */, -1, -1});
    PQsetResultAttrs(r, int(attrs.size()), attrs.data());
    int row = 0;
    for (const auto &values : rows) {
        int c = 0;
        for (const char *v : values) {
            PQsetvalue(r, row, c++, const_cast<char *>(v), v ? int(std::strlen(v)) : -1);
        }
        ++row;
    }
    return Result(r);
}

} // namespace

class TestRowStore : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void storesChunks()
    {
        RowStore store;
        store.append(makeResult({"id", "name"}, {{"1", "one"}, {"2", nullptr}}, 1234));
        store.append(makeResult({"id", "name"}, {{"3", ""}}));
        store.append(makeResult({"id", "name"}, {})); // The final, empty result of chunked mode.

        QCOMPARE(store.columnCount(), 2);
        QCOMPARE(store.rowCount(), 3);
        QCOMPARE(store.column(1).name, QStringLiteral("name"));
        QCOMPARE(store.column(0).table, 1234u);
        QCOMPARE(store.column(1).tableColumn, 2);
        QCOMPARE(store.value(0, 1), QByteArrayView("one"));
        QVERIFY(store.isNull(1, 1));
        QVERIFY(!store.isNull(2, 1));
        QCOMPARE(store.value(2, 1), QByteArrayView(""));
        QCOMPARE(store.value(2, 0), QByteArrayView("3"));
        QVERIFY(store.memoryUsed() > 0);
    }

    void outlivesTheResults()
    {
        RowStore store;
        {
            Result r = makeResult({"x"}, {{"kept after the result is gone"}});
            store.append(r);
        }
        QCOMPARE(store.value(0, 0), QByteArrayView("kept after the result is gone"));
        store.clear();
        QCOMPARE(store.rowCount(), 0);
        QVERIFY(!store.hasColumns());
    }

    void utf8()
    {
        RowStore store;
        store.append(makeResult({"t"}, {{"žluťoučký"}}));
        QCOMPARE(QString::fromUtf8(store.value(0, 0)), QStringLiteral("žluťoučký"));
    }
};

QTEST_GUILESS_MAIN(TestRowStore)
#include "tst_rowstore.moc"
