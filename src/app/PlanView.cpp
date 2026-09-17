// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlanView.h"

#include "ResultTextView.h"

#include <QColor>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeView>
#include <QVBoxLayout>

namespace slonisko {

using catalog::PlanNode;

void PlanModel::setPlan(const catalog::Plan &plan)
{
    beginResetModel();
    m_plan = plan;
    m_items.clear();
    flatten(m_plan.root, -1, 0);
    endResetModel();
}

void PlanModel::clear()
{
    beginResetModel();
    m_plan = {};
    m_items.clear();
    endResetModel();
}

void PlanModel::flatten(const PlanNode &node, int parent, int row)
{
    const int self = int(m_items.size());
    m_items.push_back({&node, parent, row, {}});
    if (parent >= 0)
        m_items[std::size_t(parent)].children.push_back(self);
    for (int i = 0; i < int(node.children.size()); ++i)
        flatten(node.children[std::size_t(i)], self, i);
}

QModelIndex PlanModel::index(int row, int column, const QModelIndex &parent) const
{
    if (m_items.empty() || row < 0 || column < 0 || column >= ColumnCount)
        return {};
    if (!parent.isValid())
        return row == 0 ? createIndex(0, column, quintptr(0)) : QModelIndex();
    const Item &p = m_items[parent.internalId()];
    if (row >= int(p.children.size()))
        return {};
    return createIndex(row, column, quintptr(p.children[std::size_t(row)]));
}

QModelIndex PlanModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const int parent = m_items[child.internalId()].parent;
    if (parent < 0)
        return {};
    return createIndex(m_items[std::size_t(parent)].row, 0, quintptr(parent));
}

int PlanModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return m_items.empty() ? 0 : 1;
    if (parent.column() > 0)
        return 0;
    return int(m_items[parent.internalId()].children.size());
}

int PlanModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant PlanModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Operation:
        return tr("Operation");
    case Object:
        return tr("Object");
    case Cost:
        return tr("Cost");
    case EstimatedRows:
        return tr("Est. rows");
    case ActualRows:
        return tr("Rows");
    case Loops:
        return tr("Loops");
    case TimeMs:
        return tr("Time (ms)");
    case Share:
        return tr("Own share");
    case Details:
        return tr("Details");
    default:
        return {};
    }
}

QVariant PlanModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const PlanNode &n = *m_items[index.internalId()].node;
    const int column = index.column();

    if (role == Qt::DisplayRole) {
        switch (column) {
        case Operation:
            return n.nodeType;
        case Object:
            return n.object;
        case Cost:
            return QStringLiteral("%1..%2")
                .arg(n.startupCost, 0, 'f', 2)
                .arg(n.totalCost, 0, 'f', 2);
        case EstimatedRows:
            return QString::number(n.planRows, 'f', 0);
        case ActualRows:
            return n.actualRows ? QString::number(*n.actualRows, 'f', 0) : QString();
        case Loops:
            return n.loops ? QString::number(*n.loops, 'f', 0) : QString();
        case TimeMs:
            return n.actualTotalMs ? QString::number(*n.actualTotalMs * n.loops.value_or(1), 'f', 3)
                                   : QString();
        case Share:
            return QStringLiteral("%1 %").arg(n.exclusiveShare * 100, 0, 'f', 1);
        case Details:
            return n.details.join(QStringLiteral("; "));
        default:
            return {};
        }
    }
    if (role == Qt::TextAlignmentRole && column >= Cost && column <= Share)
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::BackgroundRole && column == Share && n.exclusiveShare > 0.05)
        return QColor(220, 40, 40, int(40 + 160 * n.exclusiveShare));
    if (role == Qt::ToolTipRole) {
        if (column == Details)
            return n.details.join(QLatin1Char('\n'));
        QStringList lines;
        for (auto it = n.properties.begin(); it != n.properties.end(); ++it)
            lines << it.key() + QStringLiteral(": ") + it.value().toString();
        return lines.join(QLatin1Char('\n'));
    }
    // A row estimate far off the actual count is a common cause of bad plans.
    if (role == Qt::ForegroundRole && column == EstimatedRows && n.actualRows) {
        const double estimated = std::max(n.planRows, 1.0);
        const double actual = std::max(*n.actualRows, 1.0);
        if (estimated / actual > 10 || actual / estimated > 10)
            return QColor(200, 100, 0);
    }
    return {};
}

PlanView::PlanView(QWidget *parent)
    : QWidget(parent), m_summary(new QLabel(this)), m_tree(new QTreeView(this)),
      m_model(new PlanModel(this))
{
    m_summary->setMargin(4);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_tree->setModel(m_model);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);

    // The plan as text: Scintilla again, so a block of it can be copied.
    m_text = new ResultTextView(this);

    m_modeTabs = new QTabBar(this);
    m_modeTabs->setShape(QTabBar::RoundedWest);
    m_modeTabs->setExpanding(false);
    m_modeTabs->setDrawBase(false);
    m_modeTabs->addTab(tr("Tree"));
    m_modeTabs->addTab(tr("Text"));
    m_modeTabs->setTabToolTip(0, tr("The plan as a tree, with costs and timings"));
    m_modeTabs->setTabToolTip(1, tr("The plan as text, the way psql prints it"));
    connect(m_modeTabs, &QTabBar::currentChanged, this,
            [this](int index) { setViewMode(ViewMode(index)); });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_text);

    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(0);
    middle->addWidget(m_modeTabs, 0, Qt::AlignTop);
    middle->addWidget(m_stack, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_summary);
    layout->addLayout(middle, 1);
    showMessage(tr("Explain a statement with Ctrl+E, or Ctrl+Shift+E to also run it."));
}

void PlanView::setPlan(const catalog::Plan &plan)
{
    m_model->setPlan(plan);
    m_tree->expandAll();
    for (const int column : {PlanModel::ActualRows, PlanModel::Loops, PlanModel::TimeMs})
        m_tree->setColumnHidden(column, !plan.analyzed);
    for (int c = 0; c < PlanModel::Details; ++c)
        m_tree->resizeColumnToContents(c);

    QStringList summary;
    summary << (plan.analyzed ? tr("Executed and rolled back") : tr("Estimated plan"));
    summary << tr("total cost %1").arg(plan.root.totalCost, 0, 'f', 2);
    if (plan.planningMs)
        summary << tr("planning %1 ms").arg(*plan.planningMs, 0, 'f', 3);
    if (plan.executionMs)
        summary << tr("execution %1 ms").arg(*plan.executionMs, 0, 'f', 3);
    m_summary->setStyleSheet(QString());
    m_summary->setText(summary.join(QStringLiteral(" · ")));
    m_text->setText(catalog::planText(plan));
}

void PlanView::setViewMode(ViewMode mode)
{
    m_mode = mode;
    if (m_modeTabs->currentIndex() != int(mode))
        m_modeTabs->setCurrentIndex(int(mode));
    m_stack->setCurrentWidget(mode == ViewMode::Tree ? static_cast<QWidget *>(m_tree)
                                                     : static_cast<QWidget *>(m_text));
}

void PlanView::showMessage(const QString &text, bool error)
{
    m_model->clear();
    m_text->setText(QString());
    m_summary->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_summary->setText(text);
}

} // namespace slonisko
