// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Erd.h"

#include "pg/Result.h"

#include <algorithm>

namespace slonisko::catalog {

namespace {

QString text(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QString() : QString::fromUtf8(result.value(row, column));
}

bool flag(const pg::Result &result, int row, int column)
{
    return result.value(row, column) == "t";
}

// Nodes are ordered focus first, then by name: what the layout walks over
// must not depend on the order the server happened to return rows in.
void sortNodes(ErdGraph &graph)
{
    std::ranges::sort(graph.nodes, [&graph](const ErdNode &a, const ErdNode &b) {
        if ((a.oid == graph.focus) != (b.oid == graph.focus))
            return a.oid == graph.focus;
        const int byName = a.qualifiedName().compare(b.qualifiedName(), Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.oid < b.oid;
    });
    std::ranges::sort(graph.edges, [](const ErdEdge &a, const ErdEdge &b) {
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.from < b.from;
    });
}

} // namespace

const ErdNode *ErdGraph::node(Oid oid) const
{
    const auto it = std::ranges::find(nodes, oid, &ErdNode::oid);
    return it == nodes.end() ? nullptr : &*it;
}

const ErdPlacement *ErdLayout::placement(Oid oid) const
{
    const auto it = std::ranges::find(nodes, oid, &ErdPlacement::oid);
    return it == nodes.end() ? nullptr : &*it;
}

bool hasDiagram(ObjectKind kind)
{
    return kind == ObjectKind::Table || kind == ObjectKind::PartitionedTable
        || kind == ObjectKind::ForeignTable;
}

std::vector<QByteArray> erdQueries(Oid table)
{
    const QByteArray id = QByteArray::number(table);

    // The tables in the diagram: the one in the middle and both kinds of
    // neighbour. Only their key columns are listed, with a count of the
    // ones left out; a table with no keys still gets a row of its own.
    const QByteArray nodes = "WITH involved AS ("
                             "  SELECT "
        + id
        + "::oid AS oid"
          "  UNION SELECT c.confrelid FROM pg_constraint c"
          "    WHERE c.contype = 'f' AND c.conrelid = "
        + id
        + "  UNION SELECT c.conrelid FROM pg_constraint c"
          "    WHERE c.contype = 'f' AND c.confrelid = "
        + id
        + "), "
          "tables AS ("
          "  SELECT cl.oid, n.nspname, cl.relname FROM involved i "
          "  JOIN pg_class cl ON cl.oid = i.oid "
          "  JOIN pg_namespace n ON n.oid = cl.relnamespace), "
          "cols AS ("
          "  SELECT t.oid AS reloid, a.attnum, a.attname, "
          "    format_type(a.atttypid, a.atttypmod) AS coltype, a.attnotnull, "
          "    EXISTS (SELECT 1 FROM pg_constraint pc WHERE pc.conrelid = t.oid "
          "      AND pc.contype = 'p' AND a.attnum = ANY (pc.conkey)) AS primary_key, "
          "    EXISTS (SELECT 1 FROM pg_constraint fc WHERE fc.conrelid = t.oid "
          "      AND fc.contype = 'f' AND a.attnum = ANY (fc.conkey)) AS foreign_key "
          "  FROM tables t "
          "  JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum > 0 AND NOT a.attisdropped) "
          "SELECT t.oid, t.nspname, t.relname, c.attname, c.coltype, "
          "  coalesce(c.primary_key, false), coalesce(c.foreign_key, false), "
          "  coalesce(c.attnotnull, false), "
          "  (SELECT count(*) FROM cols o "
          "   WHERE o.reloid = t.oid AND NOT (o.primary_key OR o.foreign_key)) AS others "
          "FROM tables t "
          "LEFT JOIN cols c ON c.reloid = t.oid AND (c.primary_key OR c.foreign_key) "
          "ORDER BY t.nspname, t.relname, c.attnum";

    // The foreign keys between them, with the columns at both ends in the
    // order the constraint names them.
    const QByteArray edges
        = "SELECT c.conname, c.conrelid, c.confrelid, "
          "  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
          "   FROM unnest(c.conkey) WITH ORDINALITY k(attnum, ord) "
          "   JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.attnum), "
          "  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
          "   FROM unnest(c.confkey) WITH ORDINALITY k(attnum, ord) "
          "   JOIN pg_attribute a ON a.attrelid = c.confrelid AND a.attnum = k.attnum) "
          "FROM pg_constraint c "
          "WHERE c.contype = 'f' AND (c.conrelid = "
        + id + " OR c.confrelid = " + id
        + ") "
          "ORDER BY c.conname";

    return {nodes, edges};
}

ErdGraph parseErd(Oid table, const std::vector<pg::Result> &results)
{
    ErdGraph graph;
    graph.focus = table;
    if (results.empty())
        return graph;

    const pg::Result &nodes = results[0];
    for (int row = 0; row < nodes.rowCount(); ++row) {
        const Oid oid = nodes.value(row, 0).toUInt();
        auto it = std::ranges::find(graph.nodes, oid, &ErdNode::oid);
        if (it == graph.nodes.end()) {
            ErdNode node;
            node.oid = oid;
            node.schema = text(nodes, row, 1);
            node.name = text(nodes, row, 2);
            graph.nodes.push_back(std::move(node));
            it = std::prev(graph.nodes.end());
        }
        it->otherColumns = nodes.value(row, 8).toInt();
        if (nodes.isNull(row, 3))
            continue; // A table with no key columns at all.
        ErdColumn column;
        column.name = text(nodes, row, 3);
        column.type = text(nodes, row, 4);
        column.primaryKey = flag(nodes, row, 5);
        column.foreignKey = flag(nodes, row, 6);
        column.notNull = flag(nodes, row, 7);
        it->columns.push_back(std::move(column));
    }

    if (results.size() > 1) {
        const pg::Result &edges = results[1];
        for (int row = 0; row < edges.rowCount(); ++row) {
            ErdEdge edge;
            edge.name = text(edges, row, 0);
            edge.from = edges.value(row, 1).toUInt();
            edge.to = edges.value(row, 2).toUInt();
            edge.fromColumns = text(edges, row, 3).split(QLatin1Char(','), Qt::SkipEmptyParts);
            edge.toColumns = text(edges, row, 4).split(QLatin1Char(','), Qt::SkipEmptyParts);
            // A foreign key to a table the diagram does not show would leave
            // an edge hanging; one hop around the focus table cannot.
            if (graph.node(edge.from) && graph.node(edge.to))
                graph.edges.push_back(std::move(edge));
        }
    }
    sortNodes(graph);
    return graph;
}

QSizeF ErdMetrics::sizeOf(const ErdNode &node) const
{
    const int rows
        = maxRows > 0 ? std::min(int(node.columns.size()), maxRows) : int(node.columns.size());
    qreal width = charWidth * qreal(node.qualifiedName().size());
    for (int row = 0; row < rows; ++row) {
        const ErdColumn &column = node.columns[std::size_t(row)];
        // The type sits right of the name, with a gap between them.
        width = std::max(width, charWidth * qreal(column.name.size() + column.type.size() + 3));
    }
    width = std::clamp(width + 2 * padding, minWidth, maxWidth);
    // A row is kept free for the line saying how many columns are not shown.
    const bool more = rows < int(node.columns.size()) || node.otherColumns > 0;
    return {width, headerHeight + rowHeight * qreal(rows + (more ? 1 : 0)) + padding};
}

ErdLayout starLayout(const ErdGraph &graph, const ErdMetrics &metrics)
{
    ErdLayout layout;
    if (graph.nodes.empty())
        return layout;

    const ErdNode *focus = graph.node(graph.focus);
    if (!focus)
        focus = &graph.nodes.front(); // No focus given: the first node stands in.

    // Above: what the focus table references. Below: what references it.
    // A table doing both goes above, where its key columns are.
    std::vector<Oid> above;
    std::vector<Oid> below;
    for (const ErdEdge &edge : graph.edges) {
        if (edge.isSelfReference())
            continue; // Drawn as a loop on the box, so it needs no place.
        if (edge.from == focus->oid)
            above.push_back(edge.to);
        else if (edge.to == focus->oid)
            below.push_back(edge.from);
    }
    auto tidy = [&graph, focus](std::vector<Oid> &row) {
        std::ranges::sort(row, [&graph](Oid a, Oid b) {
            const ErdNode *left = graph.node(a);
            const ErdNode *right = graph.node(b);
            return left && right
                ? left->qualifiedName().compare(right->qualifiedName(), Qt::CaseInsensitive) < 0
                : a < b;
        });
        const auto duplicates = std::ranges::unique(row);
        row.erase(duplicates.begin(), duplicates.end());
        std::erase(row, focus->oid);
    };
    tidy(above);
    tidy(below);

    // Tables with no foreign key either way still belong in the picture.
    std::vector<Oid> loose;
    for (const ErdNode &node : graph.nodes) {
        auto listed = [](const std::vector<Oid> &row, Oid oid) {
            return std::ranges::find(row, oid) != row.end();
        };
        if (node.oid == focus->oid || listed(above, node.oid) || listed(below, node.oid))
            continue;
        loose.push_back(node.oid);
    }

    auto rowWidth = [&](const std::vector<Oid> &row) {
        qreal width = 0;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (const ErdNode *node = graph.node(row[i]))
                width += metrics.sizeOf(*node).width() + (i ? metrics.columnGap : 0);
        }
        return width;
    };
    auto rowHeight = [&](const std::vector<Oid> &row) {
        qreal height = 0;
        for (const Oid oid : row) {
            if (const ErdNode *node = graph.node(oid))
                height = std::max(height, metrics.sizeOf(*node).height());
        }
        return height;
    };

    const QSizeF focusSize = metrics.sizeOf(*focus);
    const qreal widest
        = std::max({rowWidth(above), rowWidth(below), rowWidth(loose), focusSize.width()});
    const qreal aboveHeight = rowHeight(above);
    const qreal belowHeight = rowHeight(below);

    // Rows are centred on each other, so the focus table sits in the middle
    // of the picture whatever hangs off it.
    auto place = [&](const std::vector<Oid> &row, qreal top) {
        qreal x = (widest - rowWidth(row)) / 2;
        for (const Oid oid : row) {
            const ErdNode *node = graph.node(oid);
            if (!node)
                continue;
            const QSizeF size = metrics.sizeOf(*node);
            layout.nodes.push_back({oid, QRectF(QPointF(x, top), size)});
            x += size.width() + metrics.columnGap;
        }
    };

    qreal top = 0;
    place(above, top);
    if (!above.empty())
        top += aboveHeight + metrics.rowGap;
    layout.nodes.push_back(
        {focus->oid, QRectF(QPointF((widest - focusSize.width()) / 2, top), focusSize)});
    top += focusSize.height();
    if (!below.empty()) {
        top += metrics.rowGap;
        place(below, top);
        top += belowHeight;
    }
    if (!loose.empty()) {
        top += metrics.rowGap;
        place(loose, top);
        top += rowHeight(loose);
    }

    for (const ErdPlacement &placement : layout.nodes)
        layout.bounds
            = layout.bounds.isNull() ? placement.box : layout.bounds.united(placement.box);
    return layout;
}

} // namespace slonisko::catalog
