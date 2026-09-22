// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Details.h"

#include "pg/Result.h"

#include <QCoreApplication>

namespace slonisko::catalog {

namespace {

QString text(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QString() : QString::fromUtf8(result.value(row, column));
}

// Every list query's columns become the table's columns, named by the query.
DetailTable tableOf(const QString &title, const pg::Result &result)
{
    DetailTable table;
    table.title = title;
    for (int c = 0; c < result.columnCount(); ++c)
        table.columns << result.columnName(c);
    for (int row = 0; row < result.rowCount(); ++row) {
        QStringList values;
        for (int c = 0; c < result.columnCount(); ++c)
            values << text(result, row, c);
        table.rows.push_back(values);
    }
    return table;
}

// The first query of every object returns one row: name, value, name,
// value, … which keeps the properties in the order the query lists them.
DetailTable propertiesOf(const pg::Result &result, QString *title, QString *schema)
{
    DetailTable properties;
    properties.title = QCoreApplication::translate("slonisko::catalog", "Overview");
    properties.columns << QCoreApplication::translate("slonisko::catalog", "Property")
                       << QCoreApplication::translate("slonisko::catalog", "Value");
    if (result.rowCount() == 0)
        return properties;
    for (int c = 0; c < result.columnCount(); ++c) {
        const QString name = result.columnName(c);
        const QString value = text(result, 0, c);
        if (name == QLatin1String("name") && title)
            *title = value;
        if (name == QLatin1String("schema") && schema)
            *schema = value;
        if (value.isEmpty())
            continue; // A property nothing is known about is not worth a row.
        properties.rows.push_back({name, value});
    }
    return properties;
}

const char *RelationProperties
    = "SELECT c.relname AS name, n.nspname AS schema, pg_get_userbyid(c.relowner) AS owner, "
      "  CASE c.relpersistence WHEN 't' THEN 'temporary' WHEN 'u' THEN 'unlogged' END AS "
      "persistence, "
      "  CASE WHEN c.relrowsecurity THEN 'enabled' END AS \"row security\", "
      "  t.spcname AS tablespace, "
      "  pg_size_pretty(pg_total_relation_size(c.oid)) AS size, "
      "  CASE WHEN c.reltuples >= 0 THEN c.reltuples::bigint::text END AS \"estimated rows\", "
      "  obj_description(c.oid, 'pg_class') AS comment "
      "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
      "LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.oid = ";

const char *Columns
    = "SELECT a.attnum AS \"#\", a.attname AS name, format_type(a.atttypid, a.atttypmod) AS type, "
      "  CASE WHEN a.attnotnull THEN 'not null' END AS nullable, "
      "  pg_get_expr(d.adbin, d.adrelid) AS default, "
      "  CASE a.attidentity WHEN 'a' THEN 'always' WHEN 'd' THEN 'by default' END AS identity, "
      "  col_description(a.attrelid, a.attnum) AS comment "
      "FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
      "WHERE a.attnum > 0 AND NOT a.attisdropped AND a.attrelid = ";

const char *Indexes
    = "SELECT ic.relname AS name, "
      "  CASE WHEN i.indisprimary THEN 'primary key' WHEN i.indisunique THEN 'unique' END AS kind, "
      "  pg_size_pretty(pg_relation_size(i.indexrelid)) AS size, "
      "  pg_get_indexdef(i.indexrelid) AS definition "
      "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid WHERE i.indrelid = ";

const char *Constraints
    = "SELECT c.conname AS name, "
      "  CASE c.contype WHEN 'p' THEN 'primary key' WHEN 'f' THEN 'foreign key' "
      "    WHEN 'u' THEN 'unique' WHEN 'c' THEN 'check' WHEN 'x' THEN 'exclude' END AS type, "
      "  pg_get_constraintdef(c.oid) AS definition "
      // Not-null constraints (PostgreSQL 18 and later list them here) are left
      // out: the Columns tab already says which columns are not null.
      "FROM pg_constraint c WHERE c.contype <> 'n' AND c.conrelid = ";

const char *Triggers = "SELECT t.tgname AS name, "
                       "  CASE t.tgenabled WHEN 'D' THEN 'disabled' ELSE 'enabled' END AS status, "
                       "  pg_get_triggerdef(t.oid) AS definition "
                       "FROM pg_trigger t WHERE NOT t.tgisinternal AND t.tgrelid = ";

QByteArray withOid(const char *sql, Oid oid, const char *order = nullptr)
{
    QByteArray query = QByteArray(sql) + QByteArray::number(oid);
    if (order)
        query += QByteArray(" ORDER BY ") + order;
    return query;
}

} // namespace

bool hasDetails(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Table:
    case ObjectKind::PartitionedTable:
    case ObjectKind::ForeignTable:
    case ObjectKind::View:
    case ObjectKind::MaterializedView:
    case ObjectKind::Extension:
        return true;
    default:
        return false;
    }
}

std::vector<QByteArray> detailQueries(ObjectKind kind, Oid oid)
{
    switch (kind) {
    case ObjectKind::Table:
    case ObjectKind::PartitionedTable:
    case ObjectKind::ForeignTable:
        return {withOid(RelationProperties, oid), withOid(Columns, oid, "1"),
                withOid(Indexes, oid, "1"), withOid(Constraints, oid, "2, 1"),
                withOid(Triggers, oid, "1")};
    case ObjectKind::View:
    case ObjectKind::MaterializedView:
        return {withOid(RelationProperties, oid), withOid(Columns, oid, "1"),
                withOid(Indexes, oid, "1"),
                "SELECT pg_get_viewdef(" + QByteArray::number(oid) + ", true)"};
    case ObjectKind::Extension:
        return {
            "SELECT e.extname AS name, e.extversion AS version, n.nspname AS schema, "
            "  pg_get_userbyid(e.extowner) AS owner, "
            "  CASE WHEN e.extrelocatable THEN 'yes' ELSE 'no' END AS relocatable, "
            "  a.default_version AS \"default version\", a.comment AS description "
            "FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace "
            "LEFT JOIN pg_available_extensions a ON a.name = e.extname WHERE e.oid = "
                + QByteArray::number(oid),
            "SELECT CASE d.classid WHEN 'pg_class'::regclass THEN 'relation' "
            "    WHEN 'pg_proc'::regclass THEN 'routine' WHEN 'pg_type'::regclass THEN 'type' "
            "    WHEN 'pg_operator'::regclass THEN 'operator' "
            "    WHEN 'pg_cast'::regclass THEN 'cast' ELSE d.classid::regclass::text END AS kind, "
            "  CASE d.classid WHEN 'pg_class'::regclass THEN d.objid::regclass::text "
            "    WHEN 'pg_proc'::regclass THEN d.objid::regproc::text "
            "    WHEN 'pg_type'::regclass THEN d.objid::regtype::text "
            "    ELSE d.objid::text END AS name "
            "FROM pg_depend d WHERE d.deptype = 'e' AND d.refclassid = 'pg_extension'::regclass "
            "  AND d.refobjid = "
                + QByteArray::number(oid) + " ORDER BY 1, 2"};
    default:
        return {};
    }
}

ObjectDetail parseDetail(ObjectKind kind, const std::vector<pg::Result> &results)
{
    auto at = [&results](std::size_t index) {
        return index < results.size() ? results[index] : pg::Result();
    };
    auto translate
        = [](const char *text) { return QCoreApplication::translate("slonisko::catalog", text); };

    ObjectDetail detail;
    detail.subtitle = kindName(kind);
    if (results.empty() || at(0).rowCount() == 0)
        return detail;

    QString name;
    QString schema;
    detail.properties = propertiesOf(at(0), &name, &schema);
    // An extension's schema is where its objects went, not part of its name.
    detail.title = schema.isEmpty() || kind == ObjectKind::Extension
        ? name
        : schema + QLatin1Char('.') + name;

    switch (kind) {
    case ObjectKind::Table:
    case ObjectKind::PartitionedTable:
    case ObjectKind::ForeignTable:
        detail.tables.push_back(tableOf(translate("Columns"), at(1)));
        detail.tables.push_back(tableOf(translate("Indexes"), at(2)));
        detail.tables.push_back(tableOf(translate("Constraints"), at(3)));
        detail.tables.push_back(tableOf(translate("Triggers"), at(4)));
        break;
    case ObjectKind::View:
    case ObjectKind::MaterializedView:
        detail.tables.push_back(tableOf(translate("Columns"), at(1)));
        if (at(2).rowCount() > 0)
            detail.tables.push_back(tableOf(translate("Indexes"), at(2)));
        if (at(3).rowCount() > 0)
            detail.definition = text(at(3), 0, 0);
        break;
    case ObjectKind::Extension:
        detail.tables.push_back(tableOf(translate("Objects"), at(1)));
        break;
    default:
        break;
    }
    return detail;
}

} // namespace slonisko::catalog
