// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Plan.h"

#include <QRectF>

#include <vector>

namespace slonisko::catalog {

// Where a plan's nodes go when it is drawn rather than listed: as a diagram
// of boxes and the rows flowing between them, or as a flame graph of where
// the time went: what goes where, and what is written in it. The views
// paint them.

// What a node and the ones under it took: "12.345 ms" with ANALYZE,
// "cost 123.45" without.
QString planValueText(const PlanNode &node);
// A line each: the operation and object, the figures, the details.
QString planNodeTooltip(const PlanNode &node);
// What a box in the diagram says, a line each: the operation; what it works
// on, or for a join its condition; its time or cost and its own share; the
// rows it handed on and the estimate.
QStringList planBoxLines(const PlanNode &node);
// The row estimate is off by more than ten times either way, a common
// cause of a bad plan.
bool rowsMisestimated(const PlanNode &node);

struct PlanDiagramBox
{
    const PlanNode *node = nullptr;
    int parent = -1; // Index of the parent's box; -1 for the root.
    int depth = 0;
    QRectF rect;
};

struct PlanDiagramLayout
{
    std::vector<PlanDiagramBox> boxes; // The root first, then depth first.
    QSizeF size; // Of everything, from (0, 0).
    double maxRows = 0; // The most rows any node hands on, to scale the edges by.
};

// The root at the top, each node's children in a row below it, the parent
// over the middle of them. A subtree takes the columns of its leaves, so no
// two subtrees overlap however unbalanced the plan is.
PlanDiagramLayout layoutPlanDiagram(const Plan &plan, QSizeF box, QSizeF gap);

// How thick to draw the edge carrying rows, from 1 for none to maxWidth for
// the most: on a log scale, since one join may hand on millions of rows and
// the next ten.
double edgeWidth(double rows, double maxRows, double maxWidth);

struct FlameFrame
{
    const PlanNode *node = nullptr;
    int parent = -1;
    int depth = 0; // 0 for the root, drawn at the bottom.
    // Of the root's width, 0 to 1.
    double start = 0;
    double width = 0;
    double value = 0; // inclusiveValue() of the node.
};

// A frame per node, as wide as the time spent in it and under it (the cost
// without ANALYZE); its children side by side above it, in plan order.
// Where the children add up to more than the parent, as with parallel
// workers each counting their own time, they are scaled to fit it.
std::vector<FlameFrame> flameGraph(const Plan &plan);
int flameDepth(const std::vector<FlameFrame> &frames); // Rows it takes.

} // namespace slonisko::catalog
