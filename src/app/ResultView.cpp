// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultView.h"

#include "ResultModel.h"

#include <QAction>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QToolBar>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace slonisko {

ResultView::ResultView(QWidget *parent)
    : QWidget(parent), m_title(new QLabel(this)), m_status(new QLabel(this)),
      m_refresh(new QToolButton(this)), m_stack(new QStackedWidget(this)),
      m_table(new QTableView(this)), m_message(new QLabel(this)), m_model(new ResultModel(this))
{
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_refresh->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_refresh->setToolTip(tr("Run again"));
    m_refresh->setAutoRaise(true);
    m_refresh->setEnabled(false);
    connect(m_refresh, &QToolButton::clicked, this, [this] {
        if (m_rerun)
            m_rerun();
    });

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
    m_save = action(QStringLiteral("document-save"), tr("Save"), QKeySequence::Save,
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
    header->addWidget(m_status);
    header->addWidget(m_refresh);

    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    connect(m_model, &ResultModel::rowsInserted, this, [this] {
        if (!m_sized) {
            m_sized = true;
            m_table->resizeColumnsToContents();
            // Keep very wide columns, like query texts, from taking all the space.
            for (int c = 0; c < m_model->columnCount(); ++c)
                m_table->setColumnWidth(c, std::min(m_table->columnWidth(c), 400));
        }
    });

    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setMargin(8);

    m_stack->addWidget(m_table);
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

void ResultView::rerunQuery()
{
    if (!m_runner) {
        showError(tr("Not connected"));
        return;
    }
    const quint64 generation = ++m_generation;
    m_running = true;
    m_refresh->setEnabled(false);
    m_status->setText(tr("Running…"));

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
    m_refresh->setEnabled(false);
    m_status->setText(tr("Running…"));
    m_stack->setCurrentWidget(m_table);
}

void ResultView::append(const pg::Result &result)
{
    m_model->append(result);
    if (!m_sized && m_model->hasColumns() && m_model->rowCount() == 0)
        m_table->resizeColumnsToContents(); // Headers only.
}

void ResultView::finish(const QString &status)
{
    m_running = false;
    m_refresh->setEnabled(bool(m_rerun));
    m_status->setText(status);
    Q_EMIT finished();
}

void ResultView::showError(const QString &message)
{
    setMessage(message, true);
}

void ResultView::showMessage(const QString &text)
{
    setMessage(text, false);
}

void ResultView::setRerun(std::function<void()> rerun)
{
    m_rerun = std::move(rerun);
    m_refresh->setEnabled(bool(m_rerun) && !m_running);
}

void ResultView::setMessage(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_stack->setCurrentWidget(m_message);
}

} // namespace slonisko
