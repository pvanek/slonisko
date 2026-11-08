// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Dependencies.h"

#include <QWidget>

#include <memory>

class QComboBox;
class QTreeWidget;

namespace slonisko {

class CalloutLabel;

// Everything that depends on an object, as a tree: what each needs from
// the one above it, and what a DROP would do to it. For a relation, one of
// its columns can be picked instead, to see what a type change would hit.
class DependencyView : public QWidget
{
    Q_OBJECT

public:
    explicit DependencyView(QWidget *parent = nullptr);

    void setGraph(std::shared_ptr<const catalog::DependencyGraph> graph);
    // While the graph is read, or when it could not be.
    void showMessage(const QString &text, bool error = false);
    void clear();

    QTreeWidget *tree() const { return m_tree; }
    // "The whole object" first, then the relation's columns.
    QComboBox *columnBox() const { return m_column; }
    QString summary() const;
    // The objects listed, all levels, for the tab's title.
    int count() const;

Q_SIGNALS:
    // A dependent was double-clicked.
    void objectActivated(slonisko::catalog::ObjectKind kind, unsigned int oid);

private:
    void showImpact();

    std::shared_ptr<const catalog::DependencyGraph> m_graph;
    QWidget *m_columnRow = nullptr;
    QComboBox *m_column = nullptr;
    CalloutLabel *m_summary = nullptr;
    QTreeWidget *m_tree = nullptr;
};

} // namespace slonisko
