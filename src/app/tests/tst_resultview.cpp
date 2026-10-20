// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"
#include "ResultTextView.h"
#include "ResultView.h"
#include "ValueViewer.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLocale>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QVBoxLayout>

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

    // Build hosts and bare environments run in the C locale.
    void sortsNaturallyInTheCLocale()
    {
        const QLocale previous;
        QLocale::setDefault(QLocale::c());
        ResultModel model;
        model.setResult(tables());
        model.sort(0);
        QLocale::setDefault(previous);
        QCOMPARE(shown(model, 0), QStringLiteral("a|B2|b10|c"));
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

    // Cells and tooltips show the start of a long value, cut between
    // characters; the value itself is all there.
    void longValuesAreCutBetweenCharacters()
    {
        const QByteArray euros = QString(3000, QChar(0x20AC)).toUtf8(); // Three bytes each.
        const QByteArray emoji = QString::fromUtf8("\xF0\x9F\x98\x80").repeated(600).toUtf8();
        ResultModel model;
        model.setResult(
            makeResult({{"a", Text}, {"b", Text}}, {{euros.constData(), emoji.constData()}}));

        const QString tip = model.index(0, 0).data(Qt::ToolTipRole).toString();
        QCOMPARE(tip.size(), 2001);
        QVERIFY(!tip.contains(QChar::ReplacementCharacter));
        QCOMPARE(tip.back(), QChar(0x2026));
        QCOMPARE(model.index(0, 0).data(ResultModel::ValueRole).toString().toUtf8(), euros);

        // 500 UTF-16 units are 250 of these, each a surrogate pair.
        const QString cell = model.index(0, 1).data().toString();
        QCOMPARE(cell.size(), 501);
        QVERIFY(cell.at(499).isLowSurrogate());
        QCOMPARE(model.index(0, 1).data(ResultModel::ValueRole).toString().toUtf8(), emoji);
    }

    void copiesWholeValues()
    {
        const QByteArray big = QByteArray("{\"k\": \"") + QByteArray(2000, 'x') + "\"}";
        ResultView view;
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.begin(QStringLiteral("Values"));
        view.append(makeResult({{"j", Text}, {"n", Int4}, {"t", Text}},
                               {{big.constData(), "1", "a\tb"}, {"short", nullptr, "c"}}));
        view.finish(QString());
        QTableView *table = view.table();
        table->setFocus();
        QClipboard *clipboard = QGuiApplication::clipboard();

        // One cell: the value as it is.
        table->setCurrentIndex(view.model()->index(0, 0));
        QTest::keySequence(table, QKeySequence::Copy);
        QCOMPARE(clipboard->text().toUtf8(), big);

        // A ragged selection: tab-separated, lined up, NULL empty, quoted
        // where a tab is in the value.
        QItemSelectionModel *selection = table->selectionModel();
        selection->select(view.model()->index(0, 1), QItemSelectionModel::ClearAndSelect);
        selection->select(view.model()->index(0, 2), QItemSelectionModel::Select);
        selection->select(view.model()->index(1, 1), QItemSelectionModel::Select);
        QTest::keySequence(table, QKeySequence::Copy);
        QCOMPARE(clipboard->text(), QStringLiteral("1\t\"a\tb\"\n\t"));

        // The record view shows the whole value and copies it too.
        table->setCurrentIndex(view.model()->index(0, 0));
        view.setViewMode(ResultView::ViewMode::Record);
        QTableView *record = view.recordView();
        QCOMPARE(record->model()->index(0, 1).data().toString().toUtf8(), big);
        // Wrapped, though the value has a single space in it.
        QVERIFY(record->rowHeight(0) > 10 * record->rowHeight(1));
        record->setFocus();
        record->setCurrentIndex(record->model()->index(0, 1));
        QTest::keySequence(record, QKeySequence::Copy);
        QCOMPARE(clipboard->text().toUtf8(), big);
    }

    // The keys are the grid's own: neither a Copy elsewhere in the window,
    // as an Edit menu would have, nor the view's built-in copy of the
    // current cell takes them.
    void keysStayWithTheGrid()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *view = new ResultView;
        layout->addWidget(view);
        QAction other(QStringLiteral("Copy Something Else"));
        other.setShortcuts(QKeySequence::Copy);
        window.addAction(&other);
        QSignalSpy otherCopies(&other, &QAction::triggered);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        view->begin(QStringLiteral("Values"));
        view->append(makeResult({{"a", Int4}, {"b", Int4}}, {{"1", "2"}}));
        view->finish(QString());

        QTableView *table = view->table();
        table->setFocus();
        table->selectAll();
        QGuiApplication::clipboard()->clear();
        QTest::keySequence(table, QKeySequence::Copy);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("1\t2"));
        QCOMPARE(otherCopies.count(), 0);

        QTest::keyClick(table, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY(view->findChild<ValueViewer *>());
    }

    void viewsValuesInAWindow()
    {
        constexpr Oid Jsonb = 3802;
        ResultView view;
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.begin(QStringLiteral("Values"));
        view.append(
            makeResult({{"doc", Jsonb}, {"n", Int4}}, {{R"({"a":[1,2]})", "1"}, {nullptr, "2"}}));
        view.finish(QString());
        QTableView *table = view.table();
        ResultModel *model = view.model();
        auto viewer = [&] { return view.findChild<ValueViewer *>(); };
        auto closeViewer = [&] {
            viewer()->close();
            QTRY_VERIFY(!viewer());
        };

        // Shift+Enter in the grid, for the current cell: JSON as JSON.
        table->setFocus();
        table->setCurrentIndex(model->index(0, 0));
        QTest::keyClick(table, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY(viewer());
        QCOMPARE(viewer()->windowTitle(), QStringLiteral("doc, row 1"));
        QCOMPARE(viewer()->syntax(), catalog::ValueSyntax::Json);
        QCOMPARE(viewer()->text(),
                 QStringLiteral("{\n    \"a\": [\n        1,\n        2\n    ]\n}"));
        closeViewer();

        // A NULL has nothing to show.
        QVERIFY(!view.viewValue(model->index(1, 0)));
        QVERIFY(!viewer());

        // Double-click, where it does not start editing.
        // The view only takes a double-click on the cell the press was on.
        const QPoint cell = table->visualRect(model->index(1, 1)).center();
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, cell);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, {}, cell);
        QVERIFY(viewer());
        QCOMPARE(viewer()->windowTitle(), QStringLiteral("n, row 2"));
        QCOMPARE(viewer()->text(), QStringLiteral("2"));
        closeViewer();

        // The record view: its rows are the grid's columns.
        table->setCurrentIndex(model->index(0, 1));
        view.setViewMode(ResultView::ViewMode::Record);
        QTableView *record = view.recordView();
        view.activateWindow(); // Back from the viewer, which had it.
        QVERIFY(QTest::qWaitForWindowActive(&view));
        record->setFocus();
        record->setCurrentIndex(record->model()->index(0, 1));
        QTest::keyClick(record, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY(viewer());
        QCOMPARE(viewer()->windowTitle(), QStringLiteral("doc, row 1"));
        closeViewer();
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
