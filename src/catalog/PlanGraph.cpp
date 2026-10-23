// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/PlanGraph.h"

#include <algorithm>
#include <cmath>

namespace slonisko::catalog {

namespace {

struct DiagramBuilder
{
    QSizeF box;
    QSizeF gap;
    PlanDiagramLayout layout;
    int nextColumn = 0;

    // Returns the box's index; its x is the middle of its children's.
    int place(const PlanNode &node, int parent, int depth)
    {
        const int self = int(layout.boxes.size());
        layout.boxes.push_back({&node, parent, depth, {}});
        layout.maxRows = std::max(layout.maxRows, outputRows(node));
        double x;
        if (node.children.empty()) {
            x = nextColumn++ * (box.width() + gap.width());
        } else {
            double first = 0;
            double last = 0;
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                const int child = place(node.children[i], self, depth + 1);
                const double cx = layout.boxes[std::size_t(child)].rect.x();
                if (i == 0)
                    first = cx;
                last = cx;
            }
            x = (first + last) / 2;
        }
        layout.boxes[std::size_t(self)].rect
            = QRectF(x, depth * (box.height() + gap.height()), box.width(), box.height());
        return self;
    }
};

void addFrames(const PlanNode &node, int parent, int depth, double start, double width,
               std::vector<FlameFrame> &frames)
{
    const int self = int(frames.size());
    const double value = inclusiveValue(node);
    frames.push_back({&node, parent, depth, start, width, value});
    double sum = 0;
    for (const PlanNode &child : node.children)
        sum += std::max(0.0, inclusiveValue(child));
    if (sum <= 0)
        return;
    // A child's share of the parent's width; never more than all of it.
    const double scale = width / std::max(sum, value > 0 ? value : sum);
    double x = start;
    for (const PlanNode &child : node.children) {
        const double w = std::max(0.0, inclusiveValue(child)) * scale;
        addFrames(child, self, depth + 1, x, w, frames);
        x += w;
    }
}

} // namespace

QString planValueText(const PlanNode &node)
{
    if (node.actualTotalMs)
        return QStringLiteral("%1 ms").arg(inclusiveValue(node), 0, 'f', 3);
    return QStringLiteral("cost %1").arg(node.totalCost, 0, 'f', 2);
}

QString planNodeTooltip(const PlanNode &node)
{
    QStringList lines;
    lines << (node.object.isEmpty() ? node.nodeType
                                    : node.nodeType + QStringLiteral(" on ") + node.object);
    lines << QStringLiteral("Cost: %1..%2")
                 .arg(node.startupCost, 0, 'f', 2)
                 .arg(node.totalCost, 0, 'f', 2);
    if (node.actualTotalMs) {
        lines << QStringLiteral("Time: %1 ms").arg(inclusiveValue(node), 0, 'f', 3);
        if (node.loops && *node.loops > 1)
            lines << QStringLiteral("Loops: %1").arg(qint64(*node.loops));
        lines << QStringLiteral("Rows: %1 (estimated %2)")
                     .arg(qint64(outputRows(node)))
                     .arg(qint64(node.planRows * node.loops.value_or(1)));
    } else {
        lines << QStringLiteral("Rows: %1 (estimated)").arg(qint64(node.planRows));
    }
    lines << QStringLiteral("Own share: %1 %").arg(node.exclusiveShare * 100, 0, 'f', 1);
    lines << node.details;
    return lines.join(QLatin1Char('\n'));
}

QStringList planBoxLines(const PlanNode &node)
{
    QStringList lines;
    lines << node.nodeType;
    // A join has no object; its condition says more than an empty line.
    lines << (node.object.isEmpty() ? node.details.value(0) : node.object);
    lines << planValueText(node)
            + QStringLiteral(" · own %1 %").arg(node.exclusiveShare * 100, 0, 'f', 1);
    if (node.actualRows) {
        QString rows = QStringLiteral("%1 rows, %2 estimated")
                           .arg(qint64(outputRows(node)))
                           .arg(qint64(node.planRows * node.loops.value_or(1)));
        if (node.loops && *node.loops > 1)
            rows += QStringLiteral(", %1 loops").arg(qint64(*node.loops));
        lines << rows;
    } else {
        lines << QStringLiteral("%1 rows estimated").arg(qint64(node.planRows));
    }
    return lines;
}

bool rowsMisestimated(const PlanNode &node)
{
    if (!node.actualRows)
        return false;
    const double estimated = std::max(node.planRows, 1.0);
    const double actual = std::max(*node.actualRows, 1.0);
    return estimated / actual > 10 || actual / estimated > 10;
}

PlanDiagramLayout layoutPlanDiagram(const Plan &plan, QSizeF box, QSizeF gap)
{
    DiagramBuilder builder {box, gap, {}, 0};
    if (plan.root.nodeType.isEmpty())
        return {};
    builder.place(plan.root, -1, 0);
    QRectF all;
    for (const PlanDiagramBox &b : builder.layout.boxes)
        all |= b.rect;
    builder.layout.size = QSizeF(all.right(), all.bottom());
    return builder.layout;
}

double edgeWidth(double rows, double maxRows, double maxWidth)
{
    if (rows <= 0 || maxRows <= 0)
        return 1;
    const double share = std::log10(rows + 1) / std::log10(maxRows + 1);
    return 1 + (maxWidth - 1) * std::clamp(share, 0.0, 1.0);
}

std::vector<FlameFrame> flameGraph(const Plan &plan)
{
    std::vector<FlameFrame> frames;
    if (!plan.root.nodeType.isEmpty())
        addFrames(plan.root, -1, 0, 0, 1, frames);
    return frames;
}

int flameDepth(const std::vector<FlameFrame> &frames)
{
    int depth = 0;
    for (const FlameFrame &f : frames)
        depth = std::max(depth, f.depth + 1);
    return depth;
}

} // namespace slonisko::catalog
