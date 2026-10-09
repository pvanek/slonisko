// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Export.h"
#include "pg/QueryRunner.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QAction;
class QLabel;
class QStackedWidget;
class QTabBar;
class QTableView;
class QToolButton;

namespace slonisko {

class RecordModel;
class ResultTextView;
class ResultModel;

// Shows the rows of a query, or its error. Either runs the query itself
// (run()), or is fed by whoever runs it (begin(), append(), finish()).
class ResultView : public QWidget
{
    Q_OBJECT

public:
    explicit ResultView(QWidget *parent = nullptr);

    // Runs sql and shows its last result. Refresh runs it again.
    void run(pg::QueryRunner *runner, const QString &title, const QByteArray &sql);

    void begin(const QString &title);
    void append(const pg::Result &result);
    void finish(const QString &status);
    void showError(const QString &message);
    void showMessage(const QString &text);
    // What the Run Again button does; none disables it.
    void setRerun(std::function<void()> rerun);
    // Clicking a column's header sorts by it, again the other way round, a
    // third time back to the query's own order. For rows nobody edits.
    void setSortable(bool sortable);

    // How the rows are shown.
    enum class ViewMode {
        Grid, // The editable table.
        Text, // An ASCII table, to copy into a file or a message.
        Record // One row at a time, its columns under each other.
    };
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_mode; }

    // Writes the rows the options choose. Returns false and sets error when
    // the file cannot be written.
    bool exportTo(const catalog::ExportOptions &options, const QString &path,
                  QString *error = nullptr) const;
    catalog::ExportOptions exportOptions(catalog::ExportFormat format) const;
    // The rows selected in the grid, in order.
    // The selected rows, as rows of model()->rows(), in the order shown.
    std::vector<int> selectedRows() const;
    // False when the server has more rows than were fetched, which makes
    // "all rows" an export that runs the query again.
    void setAllRowsFetched(bool all);
    bool allRowsFetched() const { return m_allFetched; }
    // Opens the export dialog, as the Export button does.
    void exportWithDialog();
    // Puts the rows on the clipboard: the selected ones, or all of them.
    void copyAs(catalog::ExportFormat format);

    ResultModel *model() const { return m_model; }
    QTableView *table() const { return m_table; }
    ResultTextView *textView() const { return m_text; }
    // The editing palette and the button that shows it.
    QToolButton *editToggle() const { return m_editToggle; }
    QWidget *editBar() const { return m_editBar; }
    QTabBar *modeTabs() const { return m_modeTabs; }
    QTableView *recordView() const { return m_record; }
    bool isRunning() const { return m_running; }
    // Cancels the running query, if it is one this view runs itself.
    void stop();
    // Columns as wide as their contents, within limits, and never narrower
    // than their header's full text.
    void sizeColumns();

Q_SIGNALS:
    void finished();
    // Every row is wanted but not all of them were fetched: whoever ran the
    // query runs it again and writes the rows as they arrive.
    void exportAllRequested(const slonisko::catalog::ExportOptions &options, const QString &path);
    // Save was asked for; whoever ran the query saves the model's changes.
    void saveRequested();

private:
    void rerunQuery();
    void showOutcome(const pg::QueryOutcome &outcome, qint64 elapsedMs);
    void setMessage(const QString &text, bool error);
    void updateEditing();
    // What the header section needs for its own text, ignoring the data.
    int headerWidth(int column) const;
    // Fills the view the current mode shows; force leaves a message behind
    // even before rows have arrived.
    void updateView(bool force = false);
    void updateRecord(); // The row the record view shows.
    void stepRecord(int by); // Moves to another row in the record view.
    void refresh();

    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_refresh = nullptr;
    QToolButton *m_stop = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTableView *m_table = nullptr;
    ResultTextView *m_text = nullptr;
    QTableView *m_record = nullptr;
    RecordModel *m_recordModel = nullptr;
    QTabBar *m_modeTabs = nullptr;
    QLabel *m_recordLabel = nullptr; // "Row 3 of 42" under the record form.
    QToolButton *m_editToggle = nullptr;
    QToolButton *m_export = nullptr;
    QAction *m_previousRow = nullptr;
    QAction *m_nextRow = nullptr;
    QLabel *m_message = nullptr;
    ResultModel *m_model = nullptr;
    QLabel *m_editLabel = nullptr;
    QWidget *m_editBar = nullptr;
    QAction *m_addRow = nullptr;
    QAction *m_deleteRows = nullptr;
    QAction *m_setNull = nullptr;
    QAction *m_save = nullptr;
    QAction *m_discard = nullptr;
    QAction *m_rerunAction = nullptr;

    std::function<void()> m_rerun;
    QPointer<pg::QueryRunner> m_runner;
    QByteArray m_sql;
    quint64 m_generation = 0; // Ignores results of queries run before the latest one.
    bool m_running = false;
    bool m_sized = false; // Columns sized to their contents once rows arrived.
    ViewMode m_mode = ViewMode::Grid;
    bool m_textStale = true; // The text view is rendered when it is shown.
    bool m_allFetched = true;
    // The text view renders this many rows at most; an export writes them all.
    static constexpr int MaxTextRows = 10000;
    // Long texts and JSON would take the whole view; headers get more room
    // because a column nobody can name is of no use.
    static constexpr int MaxContentWidth = 400;
    static constexpr int MaxHeaderWidth = 600;
};

} // namespace slonisko
