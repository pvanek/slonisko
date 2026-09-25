// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QByteArray>
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

// One table and its neighbours: what it references and what references it.
// The queries run in order; their results go to parseErd() the same way.
std::vector<QByteArray> erdQueries(Oid table);
ErdGraph parseErd(Oid table, const std::vector<pg::Result> &results);

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

struct ErdLayout
{
    std::vector<ErdPlacement> nodes;
    QRectF bounds;

    const ErdPlacement *placement(Oid oid) const;
};

// The focus table in the middle, what it references in a row above, what
// references it in a row below. Deterministic: the same graph always comes
// out the same way, whatever order the server listed things in.
ErdLayout starLayout(const ErdGraph &graph, const ErdMetrics &metrics = {});

} // namespace slonisko::catalog
