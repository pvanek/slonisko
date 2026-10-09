// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"
#include "ResultTextView.h"
#include "ResultView.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QTableView>
#include <QTest>

#include <cstring>

using namespace slonisko;

namespace {

// A result built in memory; nullptr values are NULLs.
pg::Result makeResult(std::initializer_list<std::pair<const char *, Oid>> columns,
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
    return pg::Result(r);
}

constexpr Oid Int4 = 23;
constexpr Oid Text = 25;

// What a DBA view returns: names, a count, a pg_size_pretty() size and
// a column with NULLs, in the order the query gave.
pg::Result tables()
{
    return makeResult({{"name", Text}, {"n", Int4}, {"size", Text}, {"note", Text}},
                      {{"b10", "10", "8192 B", nullptr},
                       {"B2", "2", "120 kB", "x"},
                       {"a", "-1", "1 MB", nullptr},
                       {"c", "33", "900 bytes", "y"}});
}

// A column as shown, top to bottom: "a|b|c".
QString shown(const QAbstractItemModel &model, int column)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.index(row, column).data().toString();
    return out.join(QLatin1Char('|'));
}

} // namespace

class TestResultView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sortsByWhatValuesMean()
    {
        ResultModel model;
        model.setResult(tables());

        model.sort(1); // Numbers as numbers, not "10" before "2".
        QCOMPARE(shown(model, 1), QStringLiteral("-1|2|10|33"));
        model.sort(1, Qt::DescendingOrder);
        QCOMPARE(shown(model, 1), QStringLiteral("33|10|2|-1"));

        model.sort(0); // Text naturally, whatever the case.
        QCOMPARE(shown(model, 0), QStringLiteral("a|B2|b10|c"));

        model.sort(2); // Sizes by size.
        QCOMPARE(shown(model, 2), QStringLiteral("900 bytes|8192 B|120 kB|1 MB"));

        model.sort(3); // NULLs last, both ways.
        QCOMPARE(shown(model, 3), QStringLiteral("x|y|NULL|NULL"));
        model.sort(3, Qt::DescendingOrder);
        QCOMPARE(shown(model, 3), QStringLiteral("y|x|NULL|NULL"));

        // Where a shown row is among the rows as they came.
        QCOMPARE(model.sourceRow(0), 3); // "y" came last.
        QCOMPARE(model.order().size(), std::size_t(4));

        model.sort(-1); // The query's own order again.
        QCOMPARE(shown(model, 0), QStringLiteral("b10|B2|a|c"));
        QVERIFY(model.order().empty());
    }

    void sortedRowsStaySorted()
    {
        ResultModel model;
        model.setResult(tables());
        model.sort(1);
        const QPersistentModelIndex b10 = model.index(2, 0);
        QCOMPARE(b10.data().toString(), QStringLiteral("b10"));

        // A later chunk of the same query goes to its places.
        model.append(makeResult({{"name", Text}, {"n", Int4}, {"size", Text}, {"note", Text}},
                                {{"d", "5", "1 kB", nullptr}}));
        QCOMPARE(shown(model, 1), QStringLiteral("-1|2|5|10|33"));
        model.sort(1, Qt::DescendingOrder);
        QCOMPARE(b10.data().toString(), QStringLiteral("b10")); // Moved along.
        QCOMPARE(b10.row(), 1);

        // Run again: the next result comes back in the same order.
        model.clear();
        model.setResult(tables());
        QCOMPARE(shown(model, 1), QStringLiteral("33|10|2|-1"));
    }

    // Edits are kept by a row's place: an editable result is not sorted.
    void editableRowsStayAsTheyCame()
    {
        ResultModel model;
        model.setResult(tables());
        model.sort(1);
        catalog::EditTarget target;
        target.table = QStringLiteral("t");
        target.columns = {QStringLiteral("name"), QStringLiteral("n"), {}, {}};
        target.readOnly = {false, false, false, false};
        target.key = {0};
        model.setEditTarget(target);
        QCOMPARE(shown(model, 0), QStringLiteral("b10|B2|a|c"));
        model.sort(1);
        QCOMPARE(shown(model, 0), QStringLiteral("b10|B2|a|c"));
    }

    void headerClickSortsAndTheRestFollows()
    {
        ResultView view;
        view.setSortable(true);
        view.resize(600, 300);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.begin(QStringLiteral("Tables"));
        view.append(tables());
        view.finish(QString());
        QTableView *table = view.table();
        // Not sorted until asked: the query's ORDER BY comes first.
        QCOMPARE(shown(*view.model(), 0), QStringLiteral("b10|B2|a|c"));

        QHeaderView *header = table->horizontalHeader();
        auto click = [&](int column) {
            QTest::mouseClick(
                header->viewport(), Qt::LeftButton, {},
                QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                       header->height() / 2));
        };
        click(1);
        QCOMPARE(shown(*view.model(), 1), QStringLiteral("-1|2|10|33"));
        click(1);
        QCOMPARE(shown(*view.model(), 1), QStringLiteral("33|10|2|-1"));

        // The selection means the rows shown there.
        table->selectionModel()->select(view.model()->index(0, 0),
                                        QItemSelectionModel::ClearAndSelect);
        QCOMPARE(view.selectedRows(), std::vector<int> {3}); // "c", 33.

        // Copying everything, and the text view, go in the order shown.
        table->clearSelection();
        view.copyAs(catalog::ExportFormat::Csv);
        const QStringList lines
            = QGuiApplication::clipboard()->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.value(1).section(QLatin1Char(','), 0, 0), QStringLiteral("c"));
        QCOMPARE(lines.value(4).section(QLatin1Char(','), 0, 0), QStringLiteral("a"));
        view.setViewMode(ResultView::ViewMode::Text);
        const QString text = view.textView()->text();
        QVERIFY(text.indexOf(QLatin1String("c ")) < text.indexOf(QLatin1String("b10")));
        QVERIFY(text.indexOf(QLatin1String("b10")) < text.indexOf(QLatin1String("a ")));

        click(1); // A third time: as the query gave them.
        QCOMPARE(shown(*view.model(), 0), QStringLiteral("b10|B2|a|c"));
    }

    // The editor's grid is not sortable: its rows can be edited.
    void notSortableUnlessAsked()
    {
        ResultView view;
        QVERIFY(!view.table()->isSortingEnabled());
    }
};

QTEST_MAIN(TestResultView)
#include "tst_resultview.moc"
