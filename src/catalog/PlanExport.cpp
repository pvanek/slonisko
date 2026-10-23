// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/PlanExport.h"

#include "catalog/PlanGraph.h"

#include <cmath>

namespace slonisko::catalog {

namespace {

constexpr double MaxEdgeWidth = 8;

double maxRows(const PlanNode &node)
{
    double most = outputRows(node);
    for (const PlanNode &child : node.children)
        most = std::max(most, maxRows(child));
    return most;
}

QString dotString(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out;
}

// Mermaid reads its own entity codes inside a quoted label; # starts one.
QString mermaidString(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('#'), QStringLiteral("#35;"));
    out.replace(QLatin1Char('"'), QStringLiteral("#quot;"));
    out.replace(QLatin1Char('<'), QStringLiteral("#lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("#gt;"));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out;
}

QString labelLines(const PlanNode &node, const QString &separator,
                   QString (*escape)(const QString &))
{
    QStringList lines;
    for (const QString &line : planBoxLines(node)) {
        if (!line.isEmpty())
            lines << escape(line);
    }
    return lines.join(separator);
}

void dotNode(const PlanNode &node, int &next, double most, QStringList &out, int parent)
{
    const int self = next++;
    // Graphviz takes "hue saturation value" from 0 to 1: yellow to red as
    // the node's own share grows, paler the less it takes.
    const double share = std::clamp(node.exclusiveShare, 0.0, 1.0);
    out << QStringLiteral("    n%1 [label=\"%2\", fillcolor=\"%3 %4 1.000\"];")
               .arg(self)
               .arg(labelLines(node, QStringLiteral("\\n"), dotString))
               .arg(55.0 / 360 * (1 - share), 0, 'f', 3)
               .arg(0.08 + 0.6 * share, 0, 'f', 3);
    if (parent >= 0) {
        const double rows = outputRows(node);
        out << QStringLiteral("    n%1 -> n%2 [penwidth=%3, tooltip=\"%4 rows\"];")
                   .arg(self)
                   .arg(parent)
                   .arg(edgeWidth(rows, most, MaxEdgeWidth), 0, 'f', 1)
                   .arg(qint64(rows));
    }
    for (const PlanNode &child : node.children)
        dotNode(child, next, most, out, self);
}

void mermaidNode(const PlanNode &node, int &next, QStringList &out, int parent)
{
    const int self = next++;
    QString line = QStringLiteral("    n%1[\"%2\"]")
                       .arg(self)
                       .arg(labelLines(node, QStringLiteral("<br/>"), mermaidString));
    if (node.exclusiveShare >= 0.2)
        line += QStringLiteral(":::hot");
    else if (node.exclusiveShare >= 0.05)
        line += QStringLiteral(":::warm");
    out << line;
    if (parent >= 0)
        out << QStringLiteral("    n%1 -->|%2 rows| n%3")
                   .arg(self)
                   .arg(qint64(outputRows(node)))
                   .arg(parent);
    for (const PlanNode &child : node.children)
        mermaidNode(child, next, out, self);
}

void foldedNode(const PlanNode &node, const QString &stack, double unit, QStringList &out)
{
    QString frame = node.object.isEmpty() ? node.nodeType
                                          : node.nodeType + QStringLiteral(" on ") + node.object;
    // The semicolon parts frames, and a line is one stack.
    frame.replace(QLatin1Char(';'), QLatin1Char(','));
    frame.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QString path = stack.isEmpty() ? frame : stack + QLatin1Char(';') + frame;
    const qint64 value = std::llround(node.exclusive * unit);
    if (value > 0)
        out << path + QLatin1Char(' ') + QString::number(value);
    for (const PlanNode &child : node.children)
        foldedNode(child, path, unit, out);
}

} // namespace

QString planToDot(const Plan &plan)
{
    if (plan.root.nodeType.isEmpty())
        return {};
    QStringList out;
    out << QStringLiteral("digraph plan {");
    // The rows flow up, from the nodes that read them to the root.
    out << QStringLiteral("    rankdir=BT;");
    out << QStringLiteral("    node [shape=box, style=\"rounded,filled\", fontname=\"sans-serif\", "
                          "fontsize=10];");
    out << QStringLiteral("    edge [arrowsize=0.6, color=\"#888888\"];");
    int next = 0;
    dotNode(plan.root, next, maxRows(plan.root), out, -1);
    out << QStringLiteral("}") << QString();
    return out.join(QLatin1Char('\n'));
}

QString planToMermaid(const Plan &plan)
{
    if (plan.root.nodeType.isEmpty())
        return {};
    QStringList out;
    out << QStringLiteral("flowchart BT");
    int next = 0;
    mermaidNode(plan.root, next, out, -1);
    out << QStringLiteral("    classDef warm fill:#fdebc8,stroke:#d08c00");
    out << QStringLiteral("    classDef hot fill:#f9c4b4,stroke:#c62828") << QString();
    return out.join(QLatin1Char('\n'));
}

QString planToFoldedStacks(const Plan &plan)
{
    if (plan.root.nodeType.isEmpty())
        return {};
    QStringList out;
    foldedNode(plan.root, {}, plan.analyzed ? 1000 : 100, out);
    out << QString();
    return out.join(QLatin1Char('\n'));
}

} // namespace slonisko::catalog
