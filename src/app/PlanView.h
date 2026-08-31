// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Plan.h"

#include <QAbstractItemModel>
#include <QWidget>

class QLabel;
class QTreeView;

namespace slonisko {

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

    void setPlan(const catalog::Plan &plan);
    void showMessage(const QString &text, bool error = false);
    PlanModel *model() const { return m_model; }

private:
    QLabel *m_summary = nullptr;
    QTreeView *m_tree = nullptr;
    PlanModel *m_model = nullptr;
};

} // namespace slonisko
