// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Plan.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace slonisko::catalog {

namespace {

// Condition-like properties shown as details, in this order.
const char *const DetailKeys[] = {
    "Hash Cond", "Merge Cond",      "Join Filter", "Index Cond", "Recheck Cond",  "TID Cond",
    "Filter",    "One-Time Filter", "Sort Key",    "Group Key",  "Presorted Key", "Cache Key",
};

QString objectOf(const QJsonObject &n)
{
    QString object;
    const QString relation = n.value(QLatin1String("Relation Name")).toString();
    if (!relation.isEmpty()) {
        const QString schema = n.value(QLatin1String("Schema")).toString();
        object = schema.isEmpty() ? relation : schema + QLatin1Char('.') + relation;
    } else {
        for (const char *key :
             {"CTE Name", "Function Name", "Subplan Name", "Table Function Name"}) {
            object = n.value(QLatin1String(key)).toString();
            if (!object.isEmpty())
                break;
        }
    }
    const QString alias = n.value(QLatin1String("Alias")).toString();
    if (!alias.isEmpty() && alias != relation && alias != object)
        object += QLatin1Char(' ') + alias;
    const QString index = n.value(QLatin1String("Index Name")).toString();
    if (!index.isEmpty())
        object = object.isEmpty() ? index : object + QStringLiteral(" using ") + index;
    return object;
}

QString typeOf(const QJsonObject &n)
{
    QString type = n.value(QLatin1String("Node Type")).toString();
    const QString join = n.value(QLatin1String("Join Type")).toString();
    // Named like EXPLAIN's text output: "Hash Left Join", "Nested Loop Anti Join".
    if (!join.isEmpty() && join != QLatin1String("Inner")) {
        if (type.endsWith(QLatin1String(" Join")))
            type.chop(5);
        type += QLatin1Char(' ') + join + QStringLiteral(" Join");
    }
    const QString strategy = n.value(QLatin1String("Strategy")).toString();
    if (type == QLatin1String("Aggregate") && !strategy.isEmpty()
        && strategy != QLatin1String("Plain"))
        type = strategy + QLatin1Char(' ') + type;
    if (n.value(QLatin1String("Parallel Aware")).toBool())
        type = QStringLiteral("Parallel ") + type;
    return type;
}

QString valueText(const QJsonValue &v)
{
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &item : v.toArray())
            parts << item.toVariant().toString();
        return parts.join(QStringLiteral(", "));
    }
    return v.toVariant().toString();
}

PlanNode parseNode(const QJsonObject &n, bool analyzed)
{
    PlanNode node;
    node.nodeType = typeOf(n);
    node.object = objectOf(n);
    node.startupCost = n.value(QLatin1String("Startup Cost")).toDouble();
    node.totalCost = n.value(QLatin1String("Total Cost")).toDouble();
    node.planRows = n.value(QLatin1String("Plan Rows")).toDouble();
    node.planWidth = n.value(QLatin1String("Plan Width")).toInt();
    if (analyzed && n.contains(QLatin1String("Actual Loops"))) {
        node.actualStartupMs = n.value(QLatin1String("Actual Startup Time")).toDouble();
        node.actualTotalMs = n.value(QLatin1String("Actual Total Time")).toDouble();
        node.actualRows = n.value(QLatin1String("Actual Rows")).toDouble();
        node.loops = n.value(QLatin1String("Actual Loops")).toDouble();
    }
    for (const char *key : DetailKeys) {
        const QJsonValue v = n.value(QLatin1String(key));
        if (!v.isUndefined())
            node.details << QString::fromLatin1(key) + QStringLiteral(": ") + valueText(v);
    }
    const double removed = n.value(QLatin1String("Rows Removed by Filter")).toDouble();
    if (removed > 0)
        node.details << QStringLiteral("Rows Removed by Filter: %1").arg(removed);

    for (auto it = n.begin(); it != n.end(); ++it) {
        if (it.key() != QLatin1String("Plans"))
            node.properties.insert(it.key(), it.value().toVariant());
    }
    for (const QJsonValue &child : n.value(QLatin1String("Plans")).toArray())
        node.children.push_back(parseNode(child.toObject(), analyzed));
    return node;
}

// Time spent in all loops of a node, or its cost when not analyzed.
double inclusive(const PlanNode &n)
{
    if (n.actualTotalMs)
        return *n.actualTotalMs * n.loops.value_or(1);
    return n.totalCost;
}

void computeExclusive(PlanNode &n, double whole)
{
    double children = 0;
    for (PlanNode &c : n.children) {
        computeExclusive(c, whole);
        // InitPlans and SubPlans run separately; they are not part of the parent's time.
        const QString relationship
            = c.properties.value(QStringLiteral("Parent Relationship")).toString();
        if (relationship != QLatin1String("InitPlan") && relationship != QLatin1String("SubPlan"))
            children += inclusive(c);
    }
    n.exclusive = std::max(0.0, inclusive(n) - children);
    n.exclusiveShare = whole > 0 ? std::min(1.0, n.exclusive / whole) : 0;
}

} // namespace

std::optional<Plan> parsePlan(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    const QJsonObject top = doc.isArray() ? doc.array().first().toObject()
                                          : doc.object(); // EXPLAIN returns [ {...} ].
    if (parseError.error != QJsonParseError::NoError || !top.contains(QLatin1String("Plan"))) {
        if (error)
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("No plan in the output");
        return std::nullopt;
    }

    Plan plan;
    const QJsonObject root = top.value(QLatin1String("Plan")).toObject();
    plan.analyzed = root.contains(QLatin1String("Actual Loops"));
    plan.root = parseNode(root, plan.analyzed);
    if (top.contains(QLatin1String("Planning Time")))
        plan.planningMs = top.value(QLatin1String("Planning Time")).toDouble();
    if (top.contains(QLatin1String("Execution Time")))
        plan.executionMs = top.value(QLatin1String("Execution Time")).toDouble();
    computeExclusive(plan.root, inclusive(plan.root));
    return plan;
}

} // namespace slonisko::catalog
