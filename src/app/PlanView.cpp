// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlanView.h"

#include "PlanGraphView.h"
#include "DiagramFile.h"
#include "ResultTextView.h"
#include "catalog/PlanExport.h"
#include "catalog/PlanGraph.h"

#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
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

const PlanNode *PlanModel::nodeAt(const QModelIndex &index) const
{
    return index.isValid() ? m_items[index.internalId()].node : nullptr;
}

QModelIndex PlanModel::indexOf(const PlanNode *node) const
{
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].node == node)
            return createIndex(m_items[i].row, 0, quintptr(i));
    }
    return {};
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
    if (role == Qt::ForegroundRole && column == EstimatedRows && catalog::rowsMisestimated(n))
        return QColor(200, 100, 0);
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
    m_modeTabs->addTab(tr("Diagram"));
    m_modeTabs->addTab(tr("Flame Graph"));
    m_modeTabs->setTabToolTip(1, tr("The plan as text, the way psql prints it"));
    m_modeTabs->setTabToolTip(
        2,
        tr("The plan as boxes, the rows flowing up from each node to the one above it; "
           "Ctrl+wheel zooms"));
    m_modeTabs->setTabToolTip(3,
                              tr("Where the time went: each node as wide as the time spent "
                                 "in it and under it; click one to widen it"));
    connect(m_modeTabs, &QTabBar::currentChanged, this,
            [this](int index) { setViewMode(ViewMode(index)); });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_text);

    m_diagram = new PlanDiagramView(this);
    m_stack->addWidget(m_diagram);
    m_flame = new FlameGraphView(this);
    auto *flameScroll = new QScrollArea(this);
    flameScroll->setWidget(m_flame);
    flameScroll->setWidgetResizable(true);
    flameScroll->setFrameShape(QFrame::NoFrame);
    flameScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_flamePage = flameScroll;
    m_stack->addWidget(m_flamePage);

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                const PlanNode *node = m_model->nodeAt(current);
                m_diagram->setCurrentNode(node);
                m_flame->setCurrentNode(node);
            });
    connect(m_diagram, &PlanDiagramView::nodeClicked, this, &PlanView::selectNode);
    connect(m_flame, &FlameGraphView::nodeClicked, this, &PlanView::selectNode);
    // Double-clicked: to the tree, with all the node's figures.
    const auto toTree = [this](const PlanNode *node) {
        selectNode(node);
        setViewMode(ViewMode::Tree);
        m_tree->setFocus();
    };
    connect(m_diagram, &PlanDiagramView::nodeActivated, this, toTree);
    connect(m_flame, &FlameGraphView::nodeActivated, this, toTree);

    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(0);
    middle->addWidget(m_modeTabs, 0, Qt::AlignTop);
    middle->addWidget(m_stack, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_header = new QHBoxLayout;
    m_header->setContentsMargins(0, 0, 4, 0);
    m_header->addWidget(m_summary, 1);
    addExportButton();
    layout->addLayout(m_header);
    layout->addLayout(middle, 1);
    showMessage(tr("Explain a statement with Ctrl+E, or Ctrl+Shift+E to also run it."));
}

void PlanView::setPlan(const catalog::Plan &plan, const QString &title)
{
    m_title = title;
    // They point into the model's plan, which is about to go.
    m_diagram->clear();
    m_flame->clear();
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
    // The model's copy, which the tree's indexes point into too.
    m_diagram->setPlan(m_model->plan());
    m_flame->setPlan(m_model->plan());
    Q_EMIT changed();
}

void PlanView::setViewMode(ViewMode mode)
{
    m_mode = mode;
    if (m_modeTabs->currentIndex() != int(mode))
        m_modeTabs->setCurrentIndex(int(mode));
    switch (mode) {
    case ViewMode::Tree:
        m_stack->setCurrentWidget(m_tree);
        break;
    case ViewMode::Text:
        m_stack->setCurrentWidget(m_text);
        break;
    case ViewMode::Diagram:
        m_stack->setCurrentWidget(m_diagram);
        break;
    case ViewMode::Flame:
        m_stack->setCurrentWidget(m_flamePage);
        break;
    }
}

void PlanView::addExportButton()
{
    // Text formats are mostly pasted into documentation or a profiler, so
    // they can go straight to the clipboard; pictures only make sense as files.
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("planExportMenu"));
    auto copy = [this, menu](const QString &objectName, const QString &text, auto toText) {
        QAction *action = menu->addAction(text);
        action->setObjectName(objectName);
        connect(action, &QAction::triggered, this, [this, toText] {
            if (hasPlan())
                QGuiApplication::clipboard()->setText(toText(m_model->plan()));
        });
    };
    auto save = [this, menu](const QStringList &filters, const QStringList &texts,
                             const QString &name, auto saveTo) {
        for (int i = 0; i < filters.size(); ++i) {
            menu->addAction(texts.value(i, filters[i]), this, [=, this] {
                if (!hasPlan())
                    return;
                exportToFile(
                    this, tr("Export Plan"), name, filters[i],
                    [=](const QString &path, QString *error) { return saveTo(path, error); });
            });
        }
    };

    menu->addSection(tr("Diagram"));
    copy(QStringLiteral("copyMermaid"), tr("Copy as Mermaid"), &catalog::planToMermaid);
    copy(QStringLiteral("copyGraphviz"), tr("Copy as Graphviz"), &catalog::planToDot);
    save(PlanDiagramView::fileFilters(),
         {tr("Save as SVG…"), tr("Save as PNG…"), tr("Save as PDF…"), tr("Save as Graphviz…"),
          tr("Save as Mermaid…")},
         QStringLiteral("plan"), [this](const QString &path, QString *error) {
             return m_diagram->saveDiagram(path, error);
         });
    menu->addSection(tr("Flame Graph"));
    copy(QStringLiteral("copyFolded"), tr("Copy as Folded Stacks"), &catalog::planToFoldedStacks);
    save(FlameGraphView::fileFilters(),
         {tr("Save as SVG…"), tr("Save as PNG…"), tr("Save as PDF…"), tr("Save as Folded Stacks…")},
         QStringLiteral("plan-flame"), [this](const QString &path, QString *error) {
             return m_flame->saveFlameGraph(path, error);
         });
    // Nothing to copy or save while a message is shown instead of a plan.
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        for (QAction *action : menu->actions())
            action->setEnabled(hasPlan());
    });

    m_exportButton = new QToolButton(this);
    m_exportButton->setIcon(QIcon::fromTheme(QStringLiteral("document-export"),
                                             QIcon::fromTheme(QStringLiteral("document-save-as"))));
    m_exportButton->setText(tr("Export"));
    m_exportButton->setToolTip(tr("Copy the plan's diagram or flame graph as text, or save "
                                  "either to a file"));
    m_exportButton->setMenu(menu);
    m_exportButton->setPopupMode(QToolButton::InstantPopup);
    m_exportButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_exportButton->setAutoRaise(true);
    m_header->addWidget(m_exportButton, 0, Qt::AlignTop);
}

void PlanView::selectNode(const PlanNode *node)
{
    const QModelIndex index = m_model->indexOf(node);
    if (index.isValid()) {
        m_tree->setCurrentIndex(index);
        m_tree->scrollTo(index);
    }
}

bool PlanView::hasPlan() const
{
    return m_model->rowCount() > 0;
}

QToolButton *PlanView::addHeaderAction(QAction *action)
{
    auto *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    m_header->addWidget(button, 0, Qt::AlignTop);
    return button;
}

void PlanView::showMessage(const QString &text, bool error)
{
    m_title.clear();
    m_diagram->clear();
    m_flame->clear();
    m_model->clear();
    m_text->setText(QString());
    m_summary->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_summary->setText(text);
    Q_EMIT changed();
}

} // namespace slonisko
