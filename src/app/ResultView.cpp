// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultView.h"

#include "ResultModel.h"
#include "ResultTextView.h"
#include "Shortcuts.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMenu>
#include <QSaveFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QToolBar>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace slonisko {

// One row of a result, its columns under each other: name, then value.
class RecordModel : public QAbstractTableModel
{
public:
    explicit RecordModel(ResultModel *rows, QObject *parent = nullptr)
        : QAbstractTableModel(parent), m_rows(rows)
    { }

    void setRow(int row)
    {
        beginResetModel();
        m_row = row;
        endResetModel();
    }
    int row() const { return m_row; }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() || m_row < 0 ? 0 : m_rows->columnCount();
    }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 2;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || m_row < 0 || m_row >= m_rows->rowCount())
            return {};
        if (index.column() == 0) {
            if (role == Qt::DisplayRole)
                return m_rows->headerData(index.row(), Qt::Horizontal);
            if (role == Qt::ForegroundRole)
                return QVariant(); // The style's own color.
            return {};
        }
        // The value, exactly as the grid shows it, NULLs and all.
        return m_rows->data(m_rows->index(m_row, index.row()), role);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        return section == 0 ? tr("Column") : tr("Value");
    }

private:
    ResultModel *m_rows = nullptr;
    int m_row = -1;
};

ResultView::ResultView(QWidget *parent)
    : QWidget(parent), m_title(new QLabel(this)), m_status(new QLabel(this)),
      m_refresh(new QToolButton(this)), m_stop(new QToolButton(this)),
      m_stack(new QStackedWidget(this)), m_table(new QTableView(this)), m_message(new QLabel(this)),
      m_model(new ResultModel(this))
{
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_refresh->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_refresh->setToolTip(tr("Run again"));
    m_refresh->setAutoRaise(true);
    // Run again: the button, F5 and Ctrl+R (Cmd+R on macOS) while in here.
    m_rerunAction
        = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Run Again"), this);
    m_rerunAction->setShortcuts(Shortcuts::refresh());
    m_rerunAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_rerunAction->setEnabled(false);
    addAction(m_rerunAction);
    connect(m_rerunAction, &QAction::triggered, this, &ResultView::refresh);
    m_refresh->setDefaultAction(m_rerunAction);
    // Stop: while a query this view runs itself is running.
    m_stop->setIcon(QIcon::fromTheme(QStringLiteral("process-stop")));
    m_stop->setText(tr("Stop"));
    m_stop->setToolTip(tr("Stop the query"));
    m_stop->setAutoRaise(true);
    m_stop->hide();
    connect(m_stop, &QToolButton::clicked, this, &ResultView::stop);

    // Editing: only shown when the result can be written back.
    m_editLabel = new QLabel(this);
    auto *editBar = new QToolBar(this);
    editBar->setIconSize(QSize(16, 16));
    editBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto action
        = [&](const QString &icon, const QString &text, const QKeySequence &key, auto slot) {
              auto *a = editBar->addAction(QIcon::fromTheme(icon), text);
              a->setShortcut(key);
              a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
              m_table->addAction(a);
              connect(a, &QAction::triggered, this, slot);
              return a;
          };
    m_addRow = action(QStringLiteral("list-add"), tr("Add Row"),
                      QKeySequence(Qt::CTRL | Qt::Key_Insert), [this] {
                          const int row = m_model->addRow();
                          m_table->setCurrentIndex(m_model->index(row, 0));
                          m_table->scrollToBottom();
                      });
    m_deleteRows
        = action(QStringLiteral("list-remove"), tr("Delete Rows"), QKeySequence::Delete, [this] {
              QList<int> rows;
              for (const QModelIndex &i : m_table->selectionModel()->selectedIndexes()) {
                  if (!rows.contains(i.row()))
                      rows << i.row();
              }
              m_model->toggleDeleted(rows);
          });
    m_setNull = action(QStringLiteral("edit-clear"), tr("Set NULL"),
                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
                       [this] { m_model->setNull(m_table->selectionModel()->selectedIndexes()); });
    // No shortcut of its own: the main window's Ctrl+S saves these changes
    // when the grid has focus, and the script otherwise.
    m_save = action(QStringLiteral("document-save"), tr("Save"), QKeySequence(),
                    [this] { Q_EMIT saveRequested(); });
    m_discard = action(QStringLiteral("edit-undo"), tr("Discard"), QKeySequence(),
                       [this] { m_model->discardChanges(); });
    m_editBar = editBar;
    connect(m_model, &ResultModel::changesChanged, this, &ResultView::updateEditing);
    connect(m_model, &ResultModel::editTargetChanged, this, &ResultView::updateEditing);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(4, 2, 4, 2);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_editLabel);
    header->addWidget(m_editBar);
    // Grid, text or one row at a time, and the same rows written out.
    m_modeBox = new QComboBox(this);
    m_modeBox->addItem(tr("Grid"), int(ViewMode::Grid));
    m_modeBox->addItem(tr("Text"), int(ViewMode::Text));
    m_modeBox->addItem(tr("Record"), int(ViewMode::Record));
    m_modeBox->setToolTip(tr("How to show the rows"));
    connect(m_modeBox, &QComboBox::activated, this,
            [this](int index) { setViewMode(ViewMode(m_modeBox->itemData(index).toInt())); });
    // Arrows from the style when the icon theme has none, so the buttons
    // stay small either way.
    m_previousRow = new QAction(
        QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_ArrowUp)),
        tr("Previous Row"), this);
    m_nextRow = new QAction(
        QIcon::fromTheme(QStringLiteral("go-down"), style()->standardIcon(QStyle::SP_ArrowDown)),
        tr("Next Row"), this);
    connect(m_previousRow, &QAction::triggered, this, [this] { stepRecord(-1); });
    connect(m_nextRow, &QAction::triggered, this, [this] { stepRecord(1); });
    auto *recordBar = new QToolBar(this);
    recordBar->setIconSize(QSize(16, 16));
    recordBar->addAction(m_previousRow);
    recordBar->addAction(m_nextRow);
    recordBar->setVisible(false);
    m_recordBar = recordBar;

    m_export = new QToolButton(this);
    m_export->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as")));
    m_export->setText(tr("Export"));
    m_export->setToolTip(tr("Save or copy the rows"));
    m_export->setAutoRaise(true);
    m_export->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(m_export);
    using catalog::ExportFormat;
    for (const ExportFormat format : {ExportFormat::Csv, ExportFormat::Tsv, ExportFormat::Text,
                                      ExportFormat::SqlInsert, ExportFormat::SqlBulkInsert}) {
        menu->addAction(tr("Save as %1…").arg(catalog::formatName(format)), this,
                        [this, format] { exportWithDialog(format); });
    }
    menu->addSeparator();
    for (const ExportFormat format :
         {ExportFormat::Csv, ExportFormat::Text, ExportFormat::SqlInsert}) {
        menu->addAction(tr("Copy as %1").arg(catalog::formatName(format)), this,
                        [this, format] { copyAs(format); });
    }
    m_export->setMenu(menu);

    header->addWidget(m_recordBar);
    header->addWidget(m_modeBox);
    header->addWidget(m_export);
    header->addWidget(m_status);
    header->addWidget(m_stop);
    header->addWidget(m_refresh);

    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    // The first chunk brings the columns and resets the model; later ones
    // insert rows. Either way the columns are sized once, when rows are there.
    auto sizeOnce = [this] {
        if (m_sized || !m_model->hasColumns())
            return;
        m_sized = m_model->rowCount() > 0; // Headers alone: size again with the rows.
        sizeColumns();
    };
    connect(m_model, &ResultModel::modelReset, this, sizeOnce);
    connect(m_model, &ResultModel::rowsInserted, this, sizeOnce);

    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setMargin(8);

    // The text view is a Scintilla: it can select a block of columns.
    m_text = new ResultTextView(this);

    m_recordModel = new RecordModel(m_model, this);
    m_record = new QTableView(this);
    m_record->setModel(m_recordModel);
    m_record->setAlternatingRowColors(true);
    m_record->verticalHeader()->hide();
    m_record->horizontalHeader()->setStretchLastSection(true);
    m_record->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_record->verticalHeader()->setDefaultSectionSize(m_record->fontMetrics().height() + 6);
    // The record view follows the grid, and the grid the record view.
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (m_mode == ViewMode::Record || current.isValid())
                    updateRecord();
            });
    connect(m_model, &ResultModel::modelReset, this, [this] {
        m_textStale = true;
        updateView();
    });
    connect(m_model, &ResultModel::rowsInserted, this, [this] { m_textStale = true; });
    connect(m_model, &ResultModel::dataChanged, this, [this] {
        m_textStale = true;
        if (m_mode == ViewMode::Record)
            updateRecord();
    });

    m_stack->addWidget(m_table);
    m_stack->addWidget(m_text);
    m_stack->addWidget(m_record);
    m_stack->addWidget(m_message);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack);

    showMessage(tr("Run a statement with Ctrl+Enter, or double-click an item under DBA Tools "
                   "or System Info."));
    updateEditing();
}

void ResultView::updateEditing()
{
    const catalog::EditTarget &target = m_model->editTarget();
    const bool editable = target.editable();
    const bool changes = m_model->hasChanges();
    m_editBar->setVisible(editable);
    m_editLabel->setVisible(m_model->hasColumns() && (editable || !target.reason.isEmpty()));
    if (editable) {
        m_editLabel->setText(tr("Editing %1.%2").arg(target.schema, target.table));
        m_editLabel->setToolTip(
            tr("Double-click a cell to change it. Changes are saved with Save."));
    } else {
        m_editLabel->setText(tr("Read-only"));
        m_editLabel->setToolTip(target.reason);
    }
    m_save->setEnabled(changes);
    m_discard->setEnabled(changes);
    m_table->setEditTriggers(editable ? QAbstractItemView::DoubleClicked
                                     | QAbstractItemView::EditKeyPressed
                                     | QAbstractItemView::AnyKeyPressed
                                      : QAbstractItemView::NoEditTriggers);
}

void ResultView::run(pg::QueryRunner *runner, const QString &title, const QByteArray &sql)
{
    m_runner = runner;
    m_sql = sql;
    m_title->setText(title);
    setRerun([this] { rerunQuery(); });
    rerunQuery();
}

void ResultView::refresh()
{
    if (!m_rerun || m_running)
        return;
    if (m_model->hasChanges()
        && QMessageBox::question(this, tr("Run Again"),
                                 tr("Running the query again discards the changes not saved yet."),
                                 QMessageBox::Discard | QMessageBox::Cancel)
            != QMessageBox::Discard)
        return;
    m_rerun();
}

void ResultView::rerunQuery()
{
    if (!m_runner) {
        showError(tr("Not connected"));
        return;
    }
    const quint64 generation = ++m_generation;
    m_running = true;
    m_rerunAction->setEnabled(false);
    m_status->setText(tr("Running…"));
    m_stop->show();
    updateView(true);

    QElapsedTimer timer;
    timer.start();
    m_runner->run(m_sql, this, [this, generation, timer](const pg::QueryOutcome &outcome) {
        if (generation == m_generation)
            showOutcome(outcome, timer.elapsed());
    });
}

void ResultView::showOutcome(const pg::QueryOutcome &outcome, qint64 elapsedMs)
{
    if (!outcome.ok()) {
        m_model->clear();
        showError(outcome.error);
    } else {
        begin(m_title->text());
        append(outcome.results.back());
    }
    finish(outcome.ok() ? tr("%n row(s), %1 ms", nullptr, m_model->rowCount()).arg(elapsedMs)
                        : tr("%1 ms").arg(elapsedMs));
}

void ResultView::begin(const QString &title)
{
    ++m_generation; // Drops the outcome of an earlier run().
    m_title->setText(title);
    m_model->clear();
    m_sized = false;
    m_running = true;
    m_rerunAction->setEnabled(false);
    m_status->setText(tr("Running…"));
    m_textStale = true;
    updateView(true); // Whatever was shown before is gone.
}

void ResultView::append(const pg::Result &result)
{
    m_model->append(result);
}

void ResultView::setViewMode(ViewMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    if (const int index = m_modeBox->findData(int(mode)); index >= 0)
        m_modeBox->setCurrentIndex(index);
    updateView();
}

void ResultView::updateView(bool force)
{
    m_recordBar->setVisible(m_mode == ViewMode::Record);
    if (!force && m_stack->currentWidget() == m_message && !m_model->hasColumns())
        return; // A message, an error or nothing run yet: no rows to show.

    switch (m_mode) {
    case ViewMode::Grid:
        m_stack->setCurrentWidget(m_table);
        return;
    case ViewMode::Text:
        if (m_textStale) {
            // Rendering a huge result would block; export writes them all.
            catalog::ExportOptions options = exportOptions(catalog::ExportFormat::Text);
            options.maxRows = MaxTextRows;
            m_text->setText(catalog::exportRows(m_model->rows(), options));
            m_textStale = false;
        }
        m_stack->setCurrentWidget(m_text);
        return;
    case ViewMode::Record:
        updateRecord();
        m_stack->setCurrentWidget(m_record);
        return;
    }
}

void ResultView::updateRecord()
{
    const QModelIndex current = m_table->currentIndex();
    const int row = current.isValid() ? current.row() : (m_model->rowCount() > 0 ? 0 : -1);
    if (row != m_recordModel->row())
        m_recordModel->setRow(row);
    m_record->resizeColumnToContents(0);
    m_previousRow->setEnabled(row > 0);
    m_nextRow->setEnabled(row >= 0 && row + 1 < m_model->rowCount());
    m_recordBar->setToolTip(row < 0 ? QString()
                                    : tr("Row %1 of %2").arg(row + 1).arg(m_model->rowCount()));
}

void ResultView::stepRecord(int by)
{
    const int row = m_recordModel->row() + by;
    if (row < 0 || row >= m_model->rowCount())
        return;
    // Through the grid, so both views stay on the same row.
    m_table->setCurrentIndex(m_model->index(row, std::max(0, m_table->currentIndex().column())));
    updateRecord();
}

catalog::ExportOptions ResultView::exportOptions(catalog::ExportFormat format) const
{
    catalog::ExportOptions options;
    options.format = format;
    const catalog::EditTarget &target = m_model->editTarget();
    if (!target.table.isEmpty()) {
        options.table = target.schema.isEmpty() ? target.table
                                                : target.schema + QLatin1Char('.') + target.table;
    }
    return options;
}

bool ResultView::exportTo(catalog::ExportFormat format, const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream out(&file);
    catalog::exportRows(m_model->rows(), exportOptions(format), out);
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void ResultView::exportWithDialog(catalog::ExportFormat format)
{
    if (!m_model->hasColumns()) {
        QMessageBox::information(this, tr("Export"), tr("There are no rows to export."));
        return;
    }
    QString name = m_title->text().simplified().replace(QLatin1Char(' '), QLatin1Char('_'));
    if (name.isEmpty())
        name = tr("result");
    const QString suggested = name + QLatin1Char('.') + catalog::fileSuffix(format);
    const QString path
        = QFileDialog::getSaveFileName(this, tr("Export as %1").arg(catalog::formatName(format)),
                                       suggested, catalog::fileFilter(format));
    if (path.isEmpty())
        return;
    if (QString error; !exportTo(format, path, &error))
        QMessageBox::warning(this, tr("Export"), tr("Could not write %1: %2").arg(path, error));
}

void ResultView::copyAs(catalog::ExportFormat format)
{
    if (!m_model->hasColumns())
        return;
    QGuiApplication::clipboard()->setText(
        catalog::exportRows(m_model->rows(), exportOptions(format)));
}

void ResultView::sizeColumns()
{
    m_table->resizeColumnsToContents();
    for (int column = 0; column < m_model->columnCount(); ++column) {
        // Contents only up to a point: a JSON document or a query text would
        // otherwise push every other column off the view. The header's own
        // text always fits, though, since a column nobody can name is of no use.
        const int head = std::min(headerWidth(column), MaxHeaderWidth);
        const int contents = std::min(m_table->columnWidth(column), MaxContentWidth);
        m_table->setColumnWidth(column, std::max(head, contents));
    }
}

int ResultView::headerWidth(int column) const
{
    QHeaderView *header = m_table->horizontalHeader();
    // sectionSizeHint() would include the data; this is the label alone.
    QStyleOptionHeader option;
    option.initFrom(header);
    option.section = column;
    option.orientation = Qt::Horizontal;
    option.text = m_model->headerData(column, Qt::Horizontal).toString();
    const QSize text = header->fontMetrics().size(0, option.text);
    return header->style()
        ->sizeFromContents(QStyle::CT_HeaderSection, &option, text, header)
        .width();
}

void ResultView::stop()
{
    if (m_running && m_runner && m_runner->connection())
        m_runner->connection()->cancel();
}

void ResultView::finish(const QString &status)
{
    m_running = false;
    m_stop->hide();
    updateView(); // The text view renders what arrived.
    m_rerunAction->setEnabled(bool(m_rerun));
    m_status->setText(status);
    Q_EMIT finished();
}

void ResultView::showError(const QString &message)
{
    m_stop->hide();
    setMessage(message, true);
}

void ResultView::showMessage(const QString &text)
{
    setMessage(text, false);
}

void ResultView::setRerun(std::function<void()> rerun)
{
    m_rerun = std::move(rerun);
    m_rerunAction->setEnabled(bool(m_rerun) && !m_running);
}

void ResultView::setMessage(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_stack->setCurrentWidget(m_message);
}

} // namespace slonisko
