// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Erd.h"

#include "pg/Result.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

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

std::vector<QPointF> ErdLayout::route(Oid from, Oid to) const
{
    const auto it = std::ranges::find_if(
        routes, [from, to](const ErdRoute &route) { return route.from == from && route.to == to; });
    return it == routes.end() ? std::vector<QPointF>() : it->bends;
}

bool hasDiagram(ObjectKind kind)
{
    return kind == ObjectKind::Table || kind == ObjectKind::PartitionedTable
        || kind == ObjectKind::ForeignTable || kind == ObjectKind::Schema;
}

std::vector<QByteArray> erdQueries(ErdScope scope, Oid oid)
{
    const QByteArray id = QByteArray::number(oid);

    // Which tables the diagram covers: one table and its neighbours, or a
    // whole schema plus whatever its keys point at.
    const QByteArray involved = scope == ErdScope::Table
        ? "WITH involved AS ("
          "  SELECT "
            + id
            + "::oid AS oid"
              "  UNION SELECT c.confrelid FROM pg_constraint c"
              "    WHERE c.contype = 'f' AND c.conrelid = "
            + id
            + "  UNION SELECT c.conrelid FROM pg_constraint c"
              "    WHERE c.contype = 'f' AND c.confrelid = "
            + id + "), "
        : "WITH mine AS ("
          "  SELECT cl.oid FROM pg_class cl WHERE cl.relnamespace = "
            + id
            + "    AND cl.relkind IN ('r', 'p', 'f')), "
              "involved AS ("
              "  SELECT oid FROM mine"
              "  UNION SELECT c.confrelid FROM pg_constraint c JOIN mine m ON m.oid = c.conrelid"
              "    WHERE c.contype = 'f'), ";

    // Only key columns are listed, with a count of the ones left out; a
    // table with no keys still gets a row of its own.
    const QByteArray nodes = involved
        + "tables AS ("
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
    const QByteArray where = scope == ErdScope::Table
        ? "c.conrelid = " + id + " OR c.confrelid = " + id
        : "c.conrelid IN (SELECT cl.oid FROM pg_class cl WHERE cl.relnamespace = " + id + ")";
    const QByteArray edges
        = "SELECT c.conname, c.conrelid, c.confrelid, "
          "  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
          "   FROM unnest(c.conkey) WITH ORDINALITY k(attnum, ord) "
          "   JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.attnum), "
          "  (SELECT string_agg(a.attname, ',' ORDER BY k.ord) "
          "   FROM unnest(c.confkey) WITH ORDINALITY k(attnum, ord) "
          "   JOIN pg_attribute a ON a.attrelid = c.confrelid AND a.attnum = k.attnum) "
          "FROM pg_constraint c "
          "WHERE c.contype = 'f' AND ("
        + where
        + ") "
          "ORDER BY c.conname";

    return {nodes, edges};
}

ErdGraph parseErd(Oid focus, const std::vector<pg::Result> &results)
{
    ErdGraph graph;
    graph.focus = focus;
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
            const ErdNode *child = graph.node(edge.from);
            if (!child || !graph.node(edge.to))
                continue;
            // Mandatory when none of the key's columns may be null.
            edge.mandatory = std::ranges::all_of(edge.fromColumns, [child](const QString &name) {
                const auto it = std::ranges::find(child->columns, name, &ErdColumn::name);
                return it != child->columns.end() && it->notNull;
            });
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

namespace {

// Sugiyama, in the order the phases are usually given: break the cycles,
// put every table on a layer, decide the order within each layer, then the
// coordinates. Everything that could depend on the order rows arrived in is
// sorted by name first, so the same schema always draws the same.
class Layered
{
public:
    Layered(const ErdGraph &graph, const ErdMetrics &metrics) : m_graph(graph), m_metrics(metrics)
    { }

    ErdLayout run()
    {
        build();
        breakCycles();
        assignLayers();
        addDummies();
        ErdLayout layout;
        // Parts that are not connected to each other are laid out on their
        // own and packed afterwards; laying them out together would let one
        // part's rows push another's about for no reason.
        qreal left = 0;
        for (const std::vector<int> &component : components()) {
            const ErdLayout part = layoutComponent(component);
            for (const ErdPlacement &placement : part.nodes)
                layout.nodes.push_back({placement.oid, placement.box.translated(left, 0)});
            for (ErdRoute route : part.routes) {
                for (QPointF &bend : route.bends)
                    bend.rx() += left;
                layout.routes.push_back(std::move(route));
            }
            left += part.bounds.width() + m_metrics.columnGap * 2;
        }
        std::ranges::sort(layout.nodes, {}, &ErdPlacement::oid);
        for (const ErdPlacement &placement : layout.nodes)
            layout.bounds
                = layout.bounds.isNull() ? placement.box : layout.bounds.united(placement.box);
        return layout;
    }

private:
    struct Vertex
    {
        Oid oid = 0; // 0 for a dummy: a place kept free for an edge to pass.
        QSizeF size;
        std::vector<int> parents; // Tables this one points at with a key.
        std::vector<int> children; // Tables pointing at this one.
        int layer = 0;
        int order = 0;
        qreal x = 0;
        qreal y = 0;
    };

    void build()
    {
        std::vector<const ErdNode *> nodes;
        for (const ErdNode &node : m_graph.nodes)
            nodes.push_back(&node);
        std::ranges::sort(nodes, [](const ErdNode *a, const ErdNode *b) {
            const int byName = a->qualifiedName().compare(b->qualifiedName(), Qt::CaseInsensitive);
            return byName != 0 ? byName < 0 : a->oid < b->oid;
        });
        for (const ErdNode *node : nodes) {
            m_index.insert(node->oid, int(m_vertices.size()));
            Vertex vertex;
            vertex.oid = node->oid;
            vertex.size = m_metrics.sizeOf(*node);
            m_vertices.push_back(std::move(vertex));
        }

        for (const ErdEdge &edge : m_graph.edges) {
            if (edge.isSelfReference())
                continue; // A loop on the box: it has no say in the layers.
            const int from = m_index.value(edge.from, -1);
            const int to = m_index.value(edge.to, -1);
            if (from < 0 || to < 0 || from == to)
                continue;
            // Several keys between the same two tables are one link here.
            if (std::ranges::find(m_vertices[std::size_t(from)].parents, to)
                == m_vertices[std::size_t(from)].parents.end()) {
                m_vertices[std::size_t(from)].parents.push_back(to);
                m_vertices[std::size_t(to)].children.push_back(from);
                // Upper end first: the table being referenced is the parent.
                m_links.push_back({to, from, {}});
            }
        }
    }

    // Depth-first, reversing the edges that lead back into the stack: after
    // this the graph is acyclic, whatever the keys do.
    void breakCycles()
    {
        std::vector<int> state(m_vertices.size(), 0); // 0 new, 1 on the stack, 2 done.
        std::vector<std::pair<int, int>> back;
        auto visit = [&](auto &&self, int v) -> void {
            state[std::size_t(v)] = 1;
            for (const int parent : m_vertices[std::size_t(v)].parents) {
                if (state[std::size_t(parent)] == 1)
                    back.emplace_back(v, parent);
                else if (state[std::size_t(parent)] == 0)
                    self(self, parent);
            }
            state[std::size_t(v)] = 2;
        };
        for (int v = 0; v < int(m_vertices.size()); ++v) {
            if (state[std::size_t(v)] == 0)
                visit(visit, v);
        }
        for (const auto &[from, to] : back) {
            std::erase(m_vertices[std::size_t(from)].parents, to);
            std::erase(m_vertices[std::size_t(to)].children, from);
            m_reversed.emplace_back(from, to);
            if (std::ranges::find(m_vertices[std::size_t(to)].parents, from)
                == m_vertices[std::size_t(to)].parents.end()) {
                m_vertices[std::size_t(to)].parents.push_back(from);
                m_vertices[std::size_t(from)].children.push_back(to);
            }
        }
    }

    // Longest path: a table sits one layer below the lowest table it points
    // at, so what is referenced is always above what references it.
    void assignLayers()
    {
        std::vector<int> pending(m_vertices.size(), 0);
        std::vector<int> ready;
        for (int v = 0; v < int(m_vertices.size()); ++v) {
            pending[std::size_t(v)] = int(m_vertices[std::size_t(v)].parents.size());
            if (pending[std::size_t(v)] == 0)
                ready.push_back(v);
        }
        for (std::size_t head = 0; head < ready.size(); ++head) {
            const int v = ready[head];
            for (const int child : m_vertices[std::size_t(v)].children) {
                Vertex &c = m_vertices[std::size_t(child)];
                c.layer = std::max(c.layer, m_vertices[std::size_t(v)].layer + 1);
                if (--pending[std::size_t(child)] == 0)
                    ready.push_back(child);
            }
        }
    }

    // An edge crossing several layers would be drawn straight through the
    // tables in between. A dummy on each layer it passes keeps a channel
    // free for it, and the layout's own ordering keeps that channel tidy.
    void addDummies()
    {
        for (Link &link : m_links) {
            const int upper = m_vertices[std::size_t(link.parent)].layer;
            const int lower = m_vertices[std::size_t(link.child)].layer;
            if (lower - upper <= 1)
                continue;
            int previous = link.parent;
            for (int layer = upper + 1; layer < lower; ++layer) {
                const int dummy = int(m_vertices.size());
                Vertex channel;
                channel.size = QSizeF(1, 1);
                channel.layer = layer;
                m_vertices.push_back(std::move(channel));
                m_vertices[std::size_t(previous)].children.push_back(dummy);
                m_vertices[std::size_t(dummy)].parents.push_back(previous);
                link.dummies.push_back(dummy);
                previous = dummy;
            }
            m_vertices[std::size_t(previous)].children.push_back(link.child);
            m_vertices[std::size_t(link.child)].parents.push_back(previous);
            // The long link itself is gone: the chain of dummies replaces it.
            std::erase(m_vertices[std::size_t(link.child)].parents, link.parent);
            std::erase(m_vertices[std::size_t(link.parent)].children, link.child);
        }
    }

    std::vector<std::vector<int>> components() const
    {
        std::vector<int> seen(m_vertices.size(), 0);
        std::vector<std::vector<int>> out;
        for (int start = 0; start < int(m_vertices.size()); ++start) {
            if (seen[std::size_t(start)])
                continue;
            std::vector<int> component {start};
            seen[std::size_t(start)] = 1;
            for (std::size_t head = 0; head < component.size(); ++head) {
                const Vertex &v = m_vertices[std::size_t(component[head])];
                for (const std::vector<int> &side : {v.parents, v.children}) {
                    for (const int next : side) {
                        if (!seen[std::size_t(next)]) {
                            seen[std::size_t(next)] = 1;
                            component.push_back(next);
                        }
                    }
                }
            }
            std::ranges::sort(component); // Which is by name: that is how they were built.
            out.push_back(std::move(component));
        }
        return out;
    }

    ErdLayout layoutComponent(const std::vector<int> &component)
    {
        // The layers of this part only, each holding its vertices in order.
        int lowest = std::numeric_limits<int>::max();
        for (const int v : component)
            lowest = std::min(lowest, m_vertices[std::size_t(v)].layer);
        std::vector<std::vector<int>> layers;
        for (const int v : component) {
            const std::size_t layer = std::size_t(m_vertices[std::size_t(v)].layer - lowest);
            if (layers.size() <= layer)
                layers.resize(layer + 1);
            layers[layer].push_back(v);
        }
        for (std::vector<int> &layer : layers) {
            for (int i = 0; i < int(layer.size()); ++i)
                m_vertices[std::size_t(layer[i])].order = i;
        }

        reduceCrossings(layers);
        return coordinates(layers);
    }

    // Repeated sweeps, each putting a vertex where the middle of what it is
    // joined to is, then swapping neighbours while that helps.
    void reduceCrossings(std::vector<std::vector<int>> &layers)
    {
        constexpr int Sweeps = 6;
        for (int sweep = 0; sweep < Sweeps; ++sweep) {
            const bool downwards = sweep % 2 == 0;
            for (int i = 0; i < int(layers.size()); ++i) {
                const int index = downwards ? i : int(layers.size()) - 1 - i;
                if ((downwards && index == 0) || (!downwards && index == int(layers.size()) - 1))
                    continue;
                orderByMedian(layers[std::size_t(index)], downwards);
            }
            for (std::size_t i = 1; i < layers.size(); ++i)
                transpose(layers[i - 1], layers[i]);
        }
    }

    void orderByMedian(std::vector<int> &layer, bool byParents)
    {
        std::vector<std::pair<qreal, int>> keys;
        for (const int v : layer) {
            const std::vector<int> &linked = byParents ? m_vertices[std::size_t(v)].parents
                                                       : m_vertices[std::size_t(v)].children;
            std::vector<int> positions;
            for (const int other : linked)
                positions.push_back(m_vertices[std::size_t(other)].order);
            std::ranges::sort(positions);
            // Nothing to go by: keep where it is, so the order stays stable.
            const qreal median = positions.empty() ? qreal(m_vertices[std::size_t(v)].order)
                                                   : qreal(positions[positions.size() / 2]);
            keys.emplace_back(median, v);
        }
        std::ranges::stable_sort(keys,
                                 [](const auto &a, const auto &b) { return a.first < b.first; });
        for (int i = 0; i < int(keys.size()); ++i) {
            layer[std::size_t(i)] = keys[std::size_t(i)].second;
            m_vertices[std::size_t(keys[std::size_t(i)].second)].order = i;
        }
    }

    int crossings(const std::vector<int> &upper, const std::vector<int> &lower) const
    {
        std::vector<std::pair<int, int>> pairs;
        for (int u = 0; u < int(upper.size()); ++u) {
            for (const int child : m_vertices[std::size_t(upper[std::size_t(u)])].children) {
                const auto it = std::ranges::find(lower, child);
                if (it != lower.end())
                    pairs.emplace_back(u, int(it - lower.begin()));
            }
        }
        int count = 0;
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            for (std::size_t j = i + 1; j < pairs.size(); ++j) {
                if ((pairs[i].first - pairs[j].first) * (pairs[i].second - pairs[j].second) < 0)
                    ++count;
            }
        }
        return count;
    }

    void transpose(std::vector<int> &upper, std::vector<int> &lower)
    {
        bool improved = true;
        while (improved) {
            improved = false;
            for (std::size_t i = 0; i + 1 < lower.size(); ++i) {
                std::vector<int> swapped = lower;
                std::swap(swapped[i], swapped[i + 1]);
                if (crossings(upper, swapped) < crossings(upper, lower)) {
                    lower = swapped;
                    improved = true;
                }
            }
        }
        for (int i = 0; i < int(lower.size()); ++i)
            m_vertices[std::size_t(lower[std::size_t(i)])].order = i;
    }

    // Boxes side by side in their layer, then a few passes pulling each one
    // towards the middle of what it is joined to, without letting them touch.
    ErdLayout coordinates(const std::vector<std::vector<int>> &layers)
    {
        for (const std::vector<int> &layer : layers) {
            qreal x = 0;
            for (const int v : layer) {
                m_vertices[std::size_t(v)].x = x;
                x += m_vertices[std::size_t(v)].size.width() + m_metrics.columnGap;
            }
        }
        for (int pass = 0; pass < 4; ++pass) {
            for (int i = 0; i < int(layers.size()); ++i) {
                const int index = pass % 2 == 0 ? i : int(layers.size()) - 1 - i;
                straighten(layers[std::size_t(index)], pass % 2 == 0);
            }
        }

        ErdLayout layout;
        std::vector<qreal> heights;
        qreal top = 0;
        for (const std::vector<int> &layer : layers) {
            qreal height = 0;
            for (const int v : layer) {
                Vertex &vertex = m_vertices[std::size_t(v)];
                vertex.y = top;
                // A dummy is a kept-free channel, not a table: it is drawn
                // as a bend in an edge, never as a box.
                if (vertex.oid != 0) {
                    layout.nodes.push_back(
                        {vertex.oid, QRectF(QPointF(vertex.x, top), vertex.size)});
                }
                height = std::max(height, vertex.size.height());
            }
            heights.push_back(height);
            top += height + m_metrics.rowGap;
        }

        // Where the edges that cross a layer have to bend: through the
        // middle of the channel their dummies kept free.
        const int firstLayer = layers.empty() || layers.front().empty()
            ? 0
            : m_vertices[std::size_t(layers.front().front())].layer;
        for (const Link &link : m_links) {
            if (link.dummies.empty())
                continue;
            const int layer = m_vertices[std::size_t(link.dummies.front())].layer - firstLayer;
            if (layer < 0 || layer >= int(layers.size()))
                continue; // Another part of the diagram; it is laid out there.
            ErdRoute route;
            route.from = m_vertices[std::size_t(link.child)].oid;
            route.to = m_vertices[std::size_t(link.parent)].oid;
            for (const int dummy : link.dummies) {
                const Vertex &v = m_vertices[std::size_t(dummy)];
                const std::size_t band = std::size_t(v.layer - firstLayer);
                route.bends.push_back({v.x, v.y + (band < heights.size() ? heights[band] : 0) / 2});
            }
            layout.routes.push_back(std::move(route));
        }

        qreal left = 0;
        for (const ErdPlacement &placement : layout.nodes)
            left = std::min(left, placement.box.left());
        for (ErdPlacement &placement : layout.nodes) {
            placement.box.translate(-left, 0);
            layout.bounds
                = layout.bounds.isNull() ? placement.box : layout.bounds.united(placement.box);
        }
        for (ErdRoute &route : layout.routes) {
            for (QPointF &bend : route.bends)
                bend.rx() -= left;
        }
        return layout;
    }

    void straighten(const std::vector<int> &layer, bool byParents)
    {
        for (int i = 0; i < int(layer.size()); ++i) {
            Vertex &v = m_vertices[std::size_t(layer[std::size_t(i)])];
            const std::vector<int> &linked = byParents ? v.parents : v.children;
            if (linked.empty())
                continue;
            std::vector<qreal> centres;
            for (const int other : linked) {
                const Vertex &o = m_vertices[std::size_t(other)];
                centres.push_back(o.x + o.size.width() / 2);
            }
            std::ranges::sort(centres);
            const qreal wanted = centres[centres.size() / 2] - v.size.width() / 2;

            // It may move only as far as its neighbours in the layer allow.
            qreal lowest = std::numeric_limits<qreal>::lowest();
            qreal highest = std::numeric_limits<qreal>::max();
            if (i > 0) {
                const Vertex &before = m_vertices[std::size_t(layer[std::size_t(i - 1)])];
                lowest = before.x + before.size.width() + m_metrics.columnGap;
            }
            if (i + 1 < int(layer.size())) {
                const Vertex &after = m_vertices[std::size_t(layer[std::size_t(i + 1)])];
                highest = after.x - v.size.width() - m_metrics.columnGap;
            }
            if (lowest <= highest)
                v.x = std::clamp(wanted, lowest, highest);
        }
    }

    struct Link
    {
        int parent = 0;
        int child = 0;
        std::vector<int> dummies; // Where it passes, layer by layer.
    };

    const ErdGraph &m_graph;
    const ErdMetrics &m_metrics;
    std::vector<Vertex> m_vertices;
    std::vector<Link> m_links;
    QHash<Oid, int> m_index;
    std::vector<std::pair<int, int>> m_reversed;
};

// Too many tables for any layout to help: name order, packed into a block.
ErdLayout gridLayout(const ErdGraph &graph, const ErdMetrics &metrics)
{
    std::vector<const ErdNode *> nodes;
    for (const ErdNode &node : graph.nodes)
        nodes.push_back(&node);
    std::ranges::sort(nodes, [](const ErdNode *a, const ErdNode *b) {
        return a->qualifiedName().compare(b->qualifiedName(), Qt::CaseInsensitive) < 0;
    });

    const int columns = std::max(1, int(std::ceil(std::sqrt(qreal(nodes.size())))));
    ErdLayout layout;
    qreal x = 0;
    qreal y = 0;
    qreal rowHeight = 0;
    for (int i = 0; i < int(nodes.size()); ++i) {
        const QSizeF size = metrics.sizeOf(*nodes[std::size_t(i)]);
        layout.nodes.push_back({nodes[std::size_t(i)]->oid, QRectF(QPointF(x, y), size)});
        rowHeight = std::max(rowHeight, size.height());
        x += size.width() + metrics.columnGap;
        if ((i + 1) % columns == 0) {
            x = 0;
            y += rowHeight + metrics.rowGap;
            rowHeight = 0;
        }
    }
    for (const ErdPlacement &placement : layout.nodes)
        layout.bounds
            = layout.bounds.isNull() ? placement.box : layout.bounds.united(placement.box);
    return layout;
}

} // namespace

ErdLayout layeredLayout(const ErdGraph &graph, const ErdMetrics &metrics)
{
    if (graph.nodes.empty())
        return {};
    if (int(graph.nodes.size()) > ManyTables)
        return gridLayout(graph, metrics);
    return Layered(graph, metrics).run();
}

ErdLayout layoutFor(const ErdGraph &graph, const ErdMetrics &metrics)
{
    return graph.focus != 0 && graph.node(graph.focus) ? starLayout(graph, metrics)
                                                       : layeredLayout(graph, metrics);
}

ErdLayout starLayout(const ErdGraph &graph, const ErdMetrics &metrics)
{
    ErdLayout layout;
    if (graph.nodes.empty())
        return layout;

    const ErdNode *focus = graph.node(graph.focus);
    if (!focus)
        focus = &graph.nodes.front(); // No focus given: the first node stands in.

    // Tables the focus points at go in the upper half, tables pointing at it
    // in the lower one; a table doing both goes above, where its keys are.
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

    // Tables with no key either way still belong in the picture; they go
    // out to the sides, where nothing else needs the room.
    std::vector<Oid> loose;
    for (const ErdNode &node : graph.nodes) {
        auto listed = [](const std::vector<Oid> &row, Oid oid) {
            return std::ranges::find(row, oid) != row.end();
        };
        if (node.oid == focus->oid || listed(above, node.oid) || listed(below, node.oid))
            continue;
        loose.push_back(node.oid);
    }

    const QSizeF focusSize = metrics.sizeOf(*focus);
    QSizeF widest(0, 0);
    for (const ErdNode &node : graph.nodes) {
        const QSizeF size = metrics.sizeOf(node);
        widest = QSizeF(std::max(widest.width(), size.width()),
                        std::max(widest.height(), size.height()));
    }

    // Round the focus table rather than in rows beside it: a row of eight
    // neighbours is a diagram nobody can see at once.
    constexpr int PerRing = 5;
    constexpr qreal HalfArc = 1.15; // Radians either side of straight up or down.
    const qreal stepX = widest.width() * 0.75 + metrics.columnGap;
    const qreal stepY = widest.height() + metrics.rowGap;
    const QPointF centre(0, 0);

    auto placeAround = [&](const std::vector<Oid> &group, qreal middle, qreal halfArc) {
        const int rings = std::max(1, int(std::ceil(qreal(group.size()) / PerRing)));
        const int perRing = int(std::ceil(qreal(group.size()) / rings));
        for (int index = 0; index < int(group.size()); ++index) {
            const ErdNode *node = graph.node(group[std::size_t(index)]);
            if (!node)
                continue;
            const int ring = index / perRing;
            const int inRing = index % perRing;
            const int count = std::min(perRing, int(group.size()) - ring * perRing);
            // One neighbour sits straight above or below; more spread out
            // evenly across the arc, in the order of their names.
            const qreal fraction = count == 1 ? 0.5 : qreal(inRing) / (count - 1);
            const qreal angle = middle - halfArc + 2 * halfArc * fraction;
            const qreal radiusX = focusSize.width() / 2 + stepX * (ring + 1);
            const qreal radiusY = focusSize.height() / 2 + stepY * (ring + 1) * 0.8;
            const QSizeF size = metrics.sizeOf(*node);
            const QPointF middlePoint(centre.x() + std::cos(angle) * radiusX,
                                      centre.y() + std::sin(angle) * radiusY);
            layout.nodes.push_back(
                {node->oid,
                 QRectF(middlePoint - QPointF(size.width() / 2, size.height() / 2), size)});
        }
    };

    layout.nodes.push_back(
        {focus->oid,
         QRectF(centre - QPointF(focusSize.width() / 2, focusSize.height() / 2), focusSize)});
    const std::size_t focusIndex = layout.nodes.size() - 1;
    placeAround(above, -M_PI_2, HalfArc); // Up is a negative angle on screen.
    placeAround(below, M_PI_2, HalfArc);
    placeAround(loose, 0, 0.6); // Off to the right, out of everyone's way.

    // Angles alone let boxes of different sizes touch; push the pairs that
    // overlap apart, away from the middle, leaving the focus where it is.
    for (int pass = 0; pass < 40; ++pass) {
        bool moved = false;
        for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
            for (std::size_t j = i + 1; j < layout.nodes.size(); ++j) {
                QRectF &a = layout.nodes[i].box;
                QRectF &b = layout.nodes[j].box;
                const QRectF gap = a.adjusted(-metrics.columnGap / 2, -metrics.rowGap / 2,
                                              metrics.columnGap / 2, metrics.rowGap / 2);
                if (!gap.intersects(b))
                    continue;
                QPointF apart = b.center() - a.center();
                if (apart.isNull())
                    apart = QPointF(1, 0);
                const qreal length = std::hypot(apart.x(), apart.y());
                const QPointF step = apart / length * (metrics.columnGap / 2 + 4);
                if (i != focusIndex)
                    a.translate(-step);
                b.translate(i == focusIndex ? step * 2 : step);
                moved = true;
            }
        }
        if (!moved)
            break;
    }

    for (const ErdPlacement &placement : layout.nodes)
        layout.bounds
            = layout.bounds.isNull() ? placement.box : layout.bounds.united(placement.box);
    // The scene starts at the origin, whatever side of the focus things fell.
    const QPointF shift = -layout.bounds.topLeft();
    for (ErdPlacement &placement : layout.nodes)
        placement.box.translate(shift);
    layout.bounds.translate(shift);
    return layout;
}

} // namespace slonisko::catalog
