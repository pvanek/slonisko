// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Objects.h"

#include "pg/Result.h"

namespace slonisko::catalog {

namespace {

using enum Folder;

// Every folder query returns these columns: oid, name, detail, subkind
// (e.g. relkind, when a folder holds several kinds) and system.
const char *sqlFor(Folder folder)
{
    switch (folder) {
    case Databases:
        return "SELECT d.oid, d.datname, NULL, NULL, false FROM pg_database d "
               "WHERE d.datallowconn AND NOT d.datistemplate ORDER BY 2";
    case Roles:
        return "SELECT r.oid, r.rolname, concat_ws(', ', "
               "CASE WHEN r.rolsuper THEN 'superuser' END, "
               "CASE WHEN r.rolcanlogin THEN 'login' END), NULL, r.rolname ~ '^pg_' "
               "FROM pg_roles r ORDER BY 5, 2";
    case Tablespaces:
        return "SELECT t.oid, t.spcname, pg_tablespace_location(t.oid), NULL, t.spcname ~ '^pg_' "
               "FROM pg_tablespace t ORDER BY 2";
    case Schemas:
        return "SELECT n.oid, n.nspname, NULL, NULL, "
               "n.nspname IN ('pg_catalog', 'information_schema') FROM pg_namespace n "
               "WHERE n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY 5, 2";
    case Extensions:
        return "SELECT e.oid, e.extname, e.extversion, NULL, false FROM pg_extension e ORDER BY 2";
    case Publications:
        return "SELECT p.oid, p.pubname, NULL, NULL, false FROM pg_publication p ORDER BY 2";
    case EventTriggers:
        return "SELECT e.oid, e.evtname, e.evtevent, NULL, false FROM pg_event_trigger e "
               "ORDER BY 2";
    case Tables:
        return "SELECT c.oid, c.relname, NULL, c.relkind::text, false FROM pg_class c "
               "WHERE c.relnamespace = %1 AND c.relkind IN ('r', 'p') AND NOT c.relispartition "
               "ORDER BY 2";
    case Views:
        return "SELECT c.oid, c.relname, NULL, NULL, false FROM pg_class c "
               "WHERE c.relnamespace = %1 AND c.relkind = 'v' ORDER BY 2";
    case MaterializedViews:
        return "SELECT c.oid, c.relname, NULL, NULL, false FROM pg_class c "
               "WHERE c.relnamespace = %1 AND c.relkind = 'm' ORDER BY 2";
    case ForeignTables:
        return "SELECT c.oid, c.relname, NULL, NULL, false FROM pg_class c "
               "WHERE c.relnamespace = %1 AND c.relkind = 'f' AND NOT c.relispartition "
               "ORDER BY 2";
    case Sequences:
        return "SELECT c.oid, c.relname, NULL, NULL, false FROM pg_class c "
               "WHERE c.relnamespace = %1 AND c.relkind = 'S' ORDER BY 2";
    case Functions:
        // Leaves out functions created along with another object, like the
        // constructors of a range type.
        return "SELECT p.oid, p.proname || '(' || pg_get_function_identity_arguments(p.oid) || "
               "')', "
               "pg_get_function_result(p.oid), NULL, false FROM pg_proc p "
               "WHERE p.pronamespace = %1 AND p.prokind IN ('f', 'w') "
               "AND NOT EXISTS (SELECT FROM pg_depend d WHERE d.classid = 'pg_proc'::regclass "
               "AND d.objid = p.oid AND d.deptype = 'i') ORDER BY 2";
    case Procedures:
        return "SELECT p.oid, p.proname || '(' || pg_get_function_identity_arguments(p.oid) || "
               "')', "
               "NULL, NULL, false FROM pg_proc p "
               "WHERE p.pronamespace = %1 AND p.prokind = 'p' ORDER BY 2";
    case Aggregates:
        return "SELECT p.oid, p.proname || '(' || pg_get_function_identity_arguments(p.oid) || "
               "')', "
               "pg_get_function_result(p.oid), NULL, false FROM pg_proc p "
               "WHERE p.pronamespace = %1 AND p.prokind = 'a' ORDER BY 2";
    case Types:
        // Leaves out the array types and row types PostgreSQL creates for
        // every type and table.
        return "SELECT t.oid, t.typname, CASE t.typtype WHEN 'b' THEN 'base' "
               "WHEN 'c' THEN 'composite' WHEN 'e' THEN 'enum' WHEN 'r' THEN 'range' "
               "WHEN 'p' THEN 'pseudo' END, NULL, false FROM pg_type t "
               "WHERE t.typnamespace = %1 AND t.typtype IN ('b', 'c', 'e', 'r', 'p') "
               "AND (t.typrelid = 0 OR "
               "(SELECT c.relkind FROM pg_class c WHERE c.oid = t.typrelid) = 'c') "
               "AND NOT EXISTS (SELECT FROM pg_type e WHERE e.oid = t.typelem "
               "AND e.typarray = t.oid) ORDER BY 2";
    case Domains:
        return "SELECT t.oid, t.typname, format_type(t.typbasetype, t.typtypmod), NULL, false "
               "FROM pg_type t WHERE t.typnamespace = %1 AND t.typtype = 'd' ORDER BY 2";
    case Columns:
        return "SELECT a.attnum, a.attname, format_type(a.atttypid, a.atttypmod) || "
               "CASE WHEN a.attnotnull THEN ' not null' ELSE '' END, NULL, false "
               "FROM pg_attribute a WHERE a.attrelid = %1 AND a.attnum > 0 "
               "AND NOT a.attisdropped ORDER BY a.attnum";
    case Constraints:
        // NOT NULL constraints (catalogued since PostgreSQL 18) show on the columns.
        return "SELECT c.oid, c.conname, CASE c.contype WHEN 'p' THEN 'primary key' "
               "WHEN 'u' THEN 'unique' WHEN 'f' THEN 'foreign key' WHEN 'c' THEN 'check' "
               "WHEN 'x' THEN 'exclusion' WHEN 't' THEN 'trigger' END, NULL, false "
               "FROM pg_constraint c WHERE c.conrelid = %1 AND c.contype <> 'n' ORDER BY 2";
    case Indexes:
        return "SELECT i.indexrelid, c.relname, CASE WHEN i.indisprimary THEN 'primary key' "
               "WHEN i.indisunique THEN 'unique' END, NULL, false FROM pg_index i "
               "JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = %1 ORDER BY 2";
    case Triggers:
        return "SELECT t.oid, t.tgname, NULL, NULL, false FROM pg_trigger t "
               "WHERE t.tgrelid = %1 AND NOT t.tgisinternal ORDER BY 2";
    case Policies:
        return "SELECT p.oid, p.polname, CASE p.polcmd WHEN '*' THEN 'all' "
               "WHEN 'r' THEN 'select' WHEN 'a' THEN 'insert' WHEN 'w' THEN 'update' "
               "WHEN 'd' THEN 'delete' END, NULL, false FROM pg_policy p "
               "WHERE p.polrelid = %1 ORDER BY 2";
    case Partitions:
        return "SELECT c.oid, c.relname, pg_get_expr(c.relpartbound, c.oid), c.relkind::text, "
               "false FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid "
               "WHERE i.inhparent = %1 ORDER BY 2";
    }
    return "";
}

ObjectKind kindIn(Folder folder, const QByteArray &subkind)
{
    switch (folder) {
    case Databases:
        return ObjectKind::Database;
    case Roles:
        return ObjectKind::Role;
    case Tablespaces:
        return ObjectKind::Tablespace;
    case Schemas:
        return ObjectKind::Schema;
    case Extensions:
        return ObjectKind::Extension;
    case Publications:
        return ObjectKind::Publication;
    case EventTriggers:
        return ObjectKind::EventTrigger;
    case Tables:
    case Partitions:
        if (subkind == "p")
            return ObjectKind::PartitionedTable;
        if (subkind == "f")
            return ObjectKind::ForeignTable;
        return ObjectKind::Table;
    case Views:
        return ObjectKind::View;
    case MaterializedViews:
        return ObjectKind::MaterializedView;
    case ForeignTables:
        return ObjectKind::ForeignTable;
    case Sequences:
        return ObjectKind::Sequence;
    case Functions:
        return ObjectKind::Function;
    case Procedures:
        return ObjectKind::Procedure;
    case Aggregates:
        return ObjectKind::Aggregate;
    case Types:
        return ObjectKind::Type;
    case Domains:
        return ObjectKind::Domain;
    case Columns:
        return ObjectKind::Column;
    case Constraints:
        return ObjectKind::Constraint;
    case Indexes:
        return ObjectKind::Index;
    case Triggers:
        return ObjectKind::Trigger;
    case Policies:
        return ObjectKind::Policy;
    }
    return ObjectKind::Table;
}

} // namespace

std::vector<Folder> foldersOf(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Database:
        return {Schemas, Extensions, Publications, EventTriggers};
    case ObjectKind::Schema:
        return {Tables,    Views,      MaterializedViews, ForeignTables, Sequences,
                Functions, Procedures, Aggregates,        Types,         Domains};
    case ObjectKind::Table:
        return {Columns, Constraints, Indexes, Triggers, Policies};
    case ObjectKind::PartitionedTable:
        return {Columns, Constraints, Indexes, Triggers, Policies, Partitions};
    case ObjectKind::View:
        return {Columns, Triggers};
    case ObjectKind::MaterializedView:
        return {Columns, Indexes};
    case ObjectKind::ForeignTable:
        return {Columns, Constraints, Triggers};
    default:
        return {};
    }
}

QString folderTitle(Folder folder)
{
    switch (folder) {
    case Databases:
        return QStringLiteral("Databases");
    case Roles:
        return QStringLiteral("Roles");
    case Tablespaces:
        return QStringLiteral("Tablespaces");
    case Schemas:
        return QStringLiteral("Schemas");
    case Extensions:
        return QStringLiteral("Extensions");
    case Publications:
        return QStringLiteral("Publications");
    case EventTriggers:
        return QStringLiteral("Event Triggers");
    case Tables:
        return QStringLiteral("Tables");
    case Views:
        return QStringLiteral("Views");
    case MaterializedViews:
        return QStringLiteral("Materialized Views");
    case ForeignTables:
        return QStringLiteral("Foreign Tables");
    case Sequences:
        return QStringLiteral("Sequences");
    case Functions:
        return QStringLiteral("Functions");
    case Procedures:
        return QStringLiteral("Procedures");
    case Aggregates:
        return QStringLiteral("Aggregates");
    case Types:
        return QStringLiteral("Types");
    case Domains:
        return QStringLiteral("Domains");
    case Columns:
        return QStringLiteral("Columns");
    case Constraints:
        return QStringLiteral("Constraints");
    case Indexes:
        return QStringLiteral("Indexes");
    case Triggers:
        return QStringLiteral("Triggers");
    case Policies:
        return QStringLiteral("Policies");
    case Partitions:
        return QStringLiteral("Partitions");
    }
    return {};
}

ObjectKind relationKind(char relkind)
{
    switch (relkind) {
    case 'p':
        return ObjectKind::PartitionedTable;
    case 'v':
        return ObjectKind::View;
    case 'm':
        return ObjectKind::MaterializedView;
    case 'f':
        return ObjectKind::ForeignTable;
    case 'S':
        return ObjectKind::Sequence;
    case 'i':
        return ObjectKind::Index;
    default:
        return ObjectKind::Table;
    }
}

QString kindName(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Database:
        return QStringLiteral("database");
    case ObjectKind::Schema:
        return QStringLiteral("schema");
    case ObjectKind::Table:
        return QStringLiteral("table");
    case ObjectKind::PartitionedTable:
        return QStringLiteral("partitioned table");
    case ObjectKind::View:
        return QStringLiteral("view");
    case ObjectKind::MaterializedView:
        return QStringLiteral("materialized view");
    case ObjectKind::ForeignTable:
        return QStringLiteral("foreign table");
    case ObjectKind::Sequence:
        return QStringLiteral("sequence");
    case ObjectKind::Function:
        return QStringLiteral("function");
    case ObjectKind::Procedure:
        return QStringLiteral("procedure");
    case ObjectKind::Aggregate:
        return QStringLiteral("aggregate");
    case ObjectKind::Type:
        return QStringLiteral("type");
    case ObjectKind::Domain:
        return QStringLiteral("domain");
    case ObjectKind::Column:
        return QStringLiteral("column");
    case ObjectKind::Constraint:
        return QStringLiteral("constraint");
    case ObjectKind::Index:
        return QStringLiteral("index");
    case ObjectKind::Trigger:
        return QStringLiteral("trigger");
    case ObjectKind::Policy:
        return QStringLiteral("policy");
    case ObjectKind::Extension:
        return QStringLiteral("extension");
    case ObjectKind::Publication:
        return QStringLiteral("publication");
    case ObjectKind::EventTrigger:
        return QStringLiteral("event trigger");
    case ObjectKind::Role:
        return QStringLiteral("role");
    case ObjectKind::Tablespace:
        return QStringLiteral("tablespace");
    }
    return {};
}

QByteArray folderQuery(Folder folder, Oid owner)
{
    return QByteArray(sqlFor(folder)).replace("%1", QByteArray::number(owner));
}

std::vector<DbObject> parseFolder(Folder folder, const pg::Result &result)
{
    std::vector<DbObject> out;
    if (result.columnCount() < 5)
        return out;
    out.reserve(std::size_t(result.rowCount()));
    for (int row = 0; row < result.rowCount(); ++row) {
        DbObject o;
        o.oid = result.value(row, 0).toUInt();
        o.name = QString::fromUtf8(result.value(row, 1));
        o.detail = QString::fromUtf8(result.value(row, 2));
        o.kind = kindIn(folder, result.value(row, 3));
        o.system = result.value(row, 4) == "t";
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace slonisko::catalog
