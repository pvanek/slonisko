// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DependencyView.h"

#include "CalloutLabel.h"
#include "Icons.h"
#include "catalog/Details.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace slonisko {

namespace {

enum Column { ObjectColumn, KindColumn, ReasonColumn, ColumnsColumn, EffectColumn };

constexpr int KindRole = Qt::UserRole;
constexpr int OidRole = Qt::UserRole + 1;

// Past this many, only the first level is open: a schema's whole contents
// unfolded is more than anyone reads.
constexpr int ExpandAllUpTo = 200;

} // namespace

DependencyView::DependencyView(QWidget *parent)
    : QWidget(parent), m_columnRow(new QWidget(this)), m_column(new QComboBox(m_columnRow)),
      m_summary(new CalloutLabel(this)), m_tree(new QTreeWidget(this))
{
    auto *columnLayout = new QHBoxLayout(m_columnRow);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    auto *columnLabel = new QLabel(tr("What depends on:"), m_columnRow);
    columnLabel->setBuddy(m_column);
    columnLayout->addWidget(columnLabel);
    columnLayout->addWidget(m_column);
    columnLayout->addStretch(1);
    m_column->setToolTip(tr("One column shows what changing its type would hit"));
    m_columnRow->hide();

    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_tree->setHeaderLabels(
        {tr("Object"), tr("Kind"), tr("Depends on it as"), tr("Columns"), tr("If dropped")});
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->addWidget(m_columnRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_tree, 1);

    connect(m_column, &QComboBox::currentIndexChanged, this, &DependencyView::showImpact);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QVariant kind = item->data(ObjectColumn, KindRole);
        if (kind.isValid())
            Q_EMIT objectActivated(kind.value<catalog::ObjectKind>(),
                                   item->data(ObjectColumn, OidRole).toUInt());
    });
}

void DependencyView::setGraph(std::shared_ptr<const catalog::DependencyGraph> graph)
{
    const QString column = m_column->currentData().toString();
    m_graph = std::move(graph);
    {
        const QSignalBlocker blocker(m_column);
        m_column->clear();
        m_column->addItem(tr("the whole object"), QString());
        for (const QString &name : m_graph->rootColumns)
            m_column->addItem(tr("column %1").arg(name), name);
        // A refresh keeps the column that was being looked at.
        m_column->setCurrentIndex(std::max(0, m_column->findData(column)));
    }
    m_columnRow->setVisible(!m_graph->rootColumns.isEmpty());
    showImpact();
}

void DependencyView::showMessage(const QString &text, bool error)
{
    clear();
    m_summary->setError(error);
    m_summary->setText(text);
}

void DependencyView::clear()
{
    m_graph.reset();
    m_tree->clear();
    m_summary->clear();
    m_columnRow->hide();
}

QString DependencyView::summary() const
{
    return m_summary->text();
}

int DependencyView::count() const
{
    int n = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        ++n;
    return n;
}

void DependencyView::showImpact()
{
    m_tree->clear();
    if (!m_graph)
        return;
    const QString column = m_column->currentData().toString();
    const catalog::Impact impact = catalog::impactOf(*m_graph, column);
    m_summary->setError(false);
    m_summary->setText(catalog::impactSummary(*m_graph, impact, !column.isEmpty()));
    // What a DROP does is beside the point of a type change.
    m_tree->setColumnHidden(EffectColumn, !column.isEmpty());

    std::vector<QTreeWidgetItem *> items;
    items.reserve(impact.dependents.size());
    for (const catalog::Dependent &d : impact.dependents) {
        const catalog::DependencyObject &object = m_graph->objects[std::size_t(d.object)];
        auto *item = d.parent < 0 ? new QTreeWidgetItem(m_tree)
                                  : new QTreeWidgetItem(items[std::size_t(d.parent)]);
        item->setText(ObjectColumn, object.identity);
        item->setText(KindColumn, object.type);
        item->setText(ReasonColumn, d.reason);
        item->setText(ColumnsColumn, d.columns.join(QStringLiteral(", ")));
        item->setText(EffectColumn, catalog::dropEffectName(d.effect));
        item->setToolTip(ObjectColumn, object.identity);
        if (const auto kind = catalog::objectKindOf(object)) {
            item->setIcon(ObjectColumn, Icons::object(*kind));
            if (catalog::hasDetails(*kind)) {
                item->setData(ObjectColumn, KindRole, QVariant::fromValue(*kind));
                item->setData(ObjectColumn, OidRole, object.oid);
            }
        }
        // What the server does not know about is easy to miss: set it apart.
        if (d.effect == catalog::DropEffect::FailsWhenRun) {
            QFont italic = item->font(ObjectColumn);
            italic.setItalic(true);
            for (int c = 0; c < m_tree->columnCount(); ++c)
                item->setFont(c, italic);
        }
        items.push_back(item);
    }
    if (items.size() <= std::size_t(ExpandAllUpTo))
        m_tree->expandAll();
    for (int c = 0; c < EffectColumn; ++c)
        m_tree->resizeColumnToContents(c);
}

} // namespace slonisko
