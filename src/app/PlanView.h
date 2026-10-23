// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Plan.h"

#include <QAbstractItemModel>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeView;

namespace slonisko {

class FlameGraphView;
class PlanDiagramView;
class ResultTextView;

// An EXPLAIN plan as a tree.
class PlanModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {
        Operation,
        Object,
        Cost,
        EstimatedRows,
        ActualRows,
        Loops,
        TimeMs,
        Share,
        Details,
        ColumnCount
    };

    using QAbstractItemModel::QAbstractItemModel;

    void setPlan(const catalog::Plan &plan);
    void clear();
    const catalog::Plan &plan() const { return m_plan; }
    const catalog::PlanNode *nodeAt(const QModelIndex &index) const;
    QModelIndex indexOf(const catalog::PlanNode *node) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    struct Item
    {
        const catalog::PlanNode *node;
        int parent; // Index into m_items, -1 for the root.
        int row;
        std::vector<int> children;
    };
    void flatten(const catalog::PlanNode &node, int parent, int row);

    catalog::Plan m_plan;
    std::vector<Item> m_items;
};

class PlanView : public QWidget
{
    Q_OBJECT

public:
    explicit PlanView(QWidget *parent = nullptr);

    // The title says what was explained, the statement as a rule.
    void setPlan(const catalog::Plan &plan, const QString &title = {});
    void showMessage(const QString &text, bool error = false);
    PlanModel *model() const { return m_model; }
    QString title() const { return m_title; }
    // A plan is on show, rather than a message.
    bool hasPlan() const;
    // A button beside the summary, for what whoever holds the view adds.
    QToolButton *addHeaderAction(QAction *action);

    // The same plan as a tree, as the text psql would print, as a diagram
    // of boxes, or as a flame graph. The node chosen in one is chosen in all.
    enum class ViewMode { Tree, Text, Diagram, Flame };
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_mode; }

    QTreeView *tree() const { return m_tree; }
    ResultTextView *textView() const { return m_text; }
    PlanDiagramView *diagram() const { return m_diagram; }
    FlameGraphView *flameGraph() const { return m_flame; }
    QTabBar *modeTabs() const { return m_modeTabs; }
    // Copies or saves the diagram or the flame graph.
    QToolButton *exportButton() const { return m_exportButton; }

Q_SIGNALS:
    // A plan or a message replaced what was shown.
    void changed();

private:
    // From the diagram or the flame graph: chosen there, chosen everywhere.
    void selectNode(const catalog::PlanNode *node);
    void addExportButton();

    QHBoxLayout *m_header = nullptr;
    QLabel *m_summary = nullptr;
    QToolButton *m_exportButton = nullptr;
    QString m_title;
    QTabBar *m_modeTabs = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTreeView *m_tree = nullptr;
    ResultTextView *m_text = nullptr;
    PlanDiagramView *m_diagram = nullptr;
    FlameGraphView *m_flame = nullptr;
    QWidget *m_flamePage = nullptr; // The flame graph in a scroll area.
    PlanModel *m_model = nullptr;
    ViewMode m_mode = ViewMode::Tree;
};

} // namespace slonisko
