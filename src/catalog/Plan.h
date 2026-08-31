// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>
#include <vector>

namespace slonisko::catalog {

// One node of an EXPLAIN (FORMAT JSON) plan.
struct PlanNode
{
    QString nodeType; // "Seq Scan", "Hash Join", ...
    QString object; // What it works on: "public.orders o", an index, a CTE, ...
    QStringList details; // Conditions and keys, like "Filter: (x > 1)".

    double startupCost = 0;
    double totalCost = 0;
    double planRows = 0;
    int planWidth = 0;

    // With ANALYZE.
    std::optional<double> actualStartupMs;
    std::optional<double> actualTotalMs; // Per loop, as PostgreSQL reports it.
    std::optional<double> actualRows; // Per loop.
    std::optional<double> loops;

    // Time (with ANALYZE) or cost spent in this node itself, not its
    // children, and its share of the whole plan's, 0 to 1.
    double exclusive = 0;
    double exclusiveShare = 0;

    QVariantMap properties; // Everything PostgreSQL said about the node.
    std::vector<PlanNode> children;
};

struct Plan
{
    PlanNode root;
    bool analyzed = false;
    std::optional<double> planningMs;
    std::optional<double> executionMs;
};

// Parses the single value EXPLAIN (FORMAT JSON) returns. On failure returns
// nullopt and sets error.
std::optional<Plan> parsePlan(const QByteArray &json, QString *error = nullptr);

} // namespace slonisko::catalog
