// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::catalog {

// Tables and the foreign keys between them, as a diagram needs them: no Qt
// Widgets anywhere near, so the layout can be tested without a display.

struct ErdColumn
{
    QString name;
    QString type;
    bool primaryKey = false;
    bool foreignKey = false;
    bool notNull = false;
};

struct ErdNode
{
    Oid oid = 0;
    QString schema;
    QString name;
    // Only the columns a key is made of: everything else is noise in a
    // diagram, and there can be a hundred of them.
    std::vector<ErdColumn> columns;
    int otherColumns = 0; // How many were left out.

    QString qualifiedName() const { return schema + QLatin1Char('.') + name; }
};

struct ErdEdge
{
    QString name; // The constraint's.
    Oid from = 0; // The table with the foreign key...
    Oid to = 0; // ...and the one it points at.
    QStringList fromColumns;
    QStringList toColumns;
    // Every column of the key is NOT NULL, so each row must have its
    // counterpart: "exactly one" rather than "zero or one" on the diagram.
    bool mandatory = false;

    bool isSelfReference() const { return from == to; }
};

struct ErdGraph
{
    Oid focus = 0; // The table the diagram was opened for, 0 for none.
    std::vector<ErdNode> nodes;
    std::vector<ErdEdge> edges;

    const ErdNode *node(Oid oid) const;
    bool isEmpty() const { return nodes.empty(); }
};

// Whether a diagram is worth showing for this kind: only relations that can
// take part in foreign keys.
bool hasDiagram(ObjectKind kind);

// What a diagram covers.
enum class ErdScope {
    Table, // One table and its neighbours: what it references and what references it.
    Schema, // Every table of a schema, and the tables their keys point at.
    Database // Every table of every schema the server does not own.
};

// The queries run in order; their results go to parseErd() the same way.
// For a schema, focus is the schema's oid and no table is singled out.
std::vector<QByteArray> erdQueries(ErdScope scope, Oid oid);
ErdGraph parseErd(Oid focus, const std::vector<pg::Result> &results);

// What a table box looks like, in scene units. The view fills this in from
// its own font; the defaults are here so layout tests need no font at all.
struct ErdMetrics
{
    qreal headerHeight = 26;
    qreal rowHeight = 18;
    qreal padding = 8;
    qreal minWidth = 140;
    qreal maxWidth = 280;
    qreal charWidth = 7; // Average character width of the column font.
    qreal columnGap = 40;
    qreal rowGap = 90;
    int maxRows = 0; // Columns to show at most; 0 shows every one.

    QSizeF sizeOf(const ErdNode &node) const;
};

struct ErdPlacement
{
    Oid oid = 0;
    QRectF box;
};

// Where an edge has to bend to get past the tables between its ends: the
// points come from the channels the layout kept free for it.
struct ErdRoute
{
    Oid from = 0;
    Oid to = 0;
    std::vector<QPointF> bends;
};

// A schema's tables, kept together and framed, in a diagram that spans more
// than one schema.
struct ErdCluster
{
    QString name;
    QRectF box;
};

struct ErdLayout
{
    std::vector<ErdPlacement> nodes;
    std::vector<ErdRoute> routes;
    std::vector<ErdCluster> clusters;
    QRectF bounds;

    const ErdPlacement *placement(Oid oid) const;
    // Empty when the edge runs between neighbouring layers, which needs no
    // help, or when the layout draws no routes at all.
    std::vector<QPointF> route(Oid from, Oid to) const;
};

// The focus table in the middle, what it references in a row above, what
// references it in a row below. Deterministic: the same graph always comes
// out the same way, whatever order the server listed things in.
ErdLayout starLayout(const ErdGraph &graph, const ErdMetrics &metrics = {});

// Tables in layers, a referenced table above the ones referencing it: cycles
// broken, layers assigned by longest path, crossings cut down by repeated
// median sweeps, and disconnected parts packed side by side. Deterministic,
// like starLayout(). Above ManyTables it falls back to a plain grid, which
// is all a diagram that size is good for anyway.
ErdLayout layeredLayout(const ErdGraph &graph, const ErdMetrics &metrics = {});
constexpr int ManyTables = 150;

// Each schema laid out on its own and framed, the blocks packed into rows:
// a whole database is a set of diagrams rather than one.
ErdLayout clusteredLayout(const ErdGraph &graph, const ErdMetrics &metrics = {});

// The layout that suits the graph: a focused table gets a star, one schema
// gets layers, several schemas get a frame each.
ErdLayout layoutFor(const ErdGraph &graph, const ErdMetrics &metrics = {});

} // namespace slonisko::catalog
