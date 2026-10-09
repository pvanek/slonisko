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

// The first query of every object returns one row, a column per property
// named after it, which keeps the properties in the order the query lists them.
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

// One query and what to do with its answer. The first section of every
// object is its properties; the rest become tabs, in this order.
struct Section
{
    const char *title = nullptr; // Null for the properties and the definition.
    QByteArray sql;
    bool definition = false; // A single value: the SQL that defines the object.
    bool hideWhenEmpty = false; // A list that is only in the way when empty.
};

QByteArray oidOf(Oid oid)
{
    return QByteArray::number(oid);
}

// Relations: tables, views, materialized views, foreign tables.

const char *RelationProperties
    = "SELECT c.relname AS name, n.nspname AS schema, pg_get_userbyid(c.relowner) AS owner, "
      "  CASE c.relpersistence WHEN 't' THEN 'temporary' WHEN 'u' THEN 'unlogged' END AS "
      "persistence, "
      "  CASE WHEN c.relrowsecurity THEN 'enabled' END AS \"row security\", "
      "  t.spcname AS tablespace, "
      "  replace(pg_size_pretty(pg_total_relation_size(c.oid)), ' bytes', ' B') AS size, "
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
      "  replace(pg_size_pretty(pg_relation_size(i.indexrelid)), ' bytes', ' B') AS size, "
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

const char *Policies
    = "SELECT p.polname AS name, "
      "  CASE p.polcmd WHEN 'r' THEN 'select' WHEN 'a' THEN 'insert' WHEN 'w' THEN 'update' "
      "    WHEN 'd' THEN 'delete' ELSE 'all' END AS command, "
      "  CASE WHEN p.polpermissive THEN 'permissive' ELSE 'restrictive' END AS kind, "
      "  pg_get_expr(p.polqual, p.polrelid) AS using, "
      "  pg_get_expr(p.polwithcheck, p.polrelid) AS \"with check\" "
      "FROM pg_policy p WHERE p.polrelid = ";

std::vector<Section> relationSections(ObjectKind kind, Oid oid)
{
    const QByteArray id = oidOf(oid);
    std::vector<Section> sections {{nullptr, RelationProperties + id},
                                   {"Columns", Columns + id + " ORDER BY 1"}};
    const bool view = kind == ObjectKind::View || kind == ObjectKind::MaterializedView;
    sections.push_back({"Indexes", Indexes + id + " ORDER BY 1", false, view});
    if (!view) {
        sections.push_back({"Constraints", Constraints + id + " ORDER BY 2, 1"});
        sections.push_back({"Triggers", Triggers + id + " ORDER BY 1"});
        sections.push_back({"Policies", Policies + id + " ORDER BY 1", false, true});
    }
    if (view)
        sections.push_back({nullptr, "SELECT pg_get_viewdef(" + id + ", true)", true});
    return sections;
}

// Everything else, kind by kind.

std::vector<Section> sectionsOf(ObjectKind kind, Oid oid)
{
    const QByteArray id = oidOf(oid);
    using enum ObjectKind;
    switch (kind) {
    case Table:
    case PartitionedTable:
    case ForeignTable:
    case View:
    case MaterializedView:
        return relationSections(kind, oid);

    case Database: {
        // The tree knows no oid for the one database a connection was made
        // to, so without one this is about the database being queried.
        const QByteArray db = oid
            ? id
            : QByteArray("(SELECT oid FROM pg_database WHERE datname = current_database())");
        return {{nullptr,
                 "SELECT d.datname AS name, pg_get_userbyid(d.datdba) AS owner, "
                 "  pg_encoding_to_char(d.encoding) AS encoding, d.datcollate AS collation, "
                 "  d.datctype AS ctype, t.spcname AS tablespace, "
                 "  CASE WHEN has_database_privilege(d.oid, 'connect') "
                 "    THEN replace(pg_size_pretty(pg_database_size(d.oid)), ' bytes', ' B') "
                 "  END AS size, "
                 "  CASE WHEN d.datconnlimit >= 0 THEN d.datconnlimit::text END AS \"connection "
                 "limit\", "
                 "  CASE WHEN NOT d.datallowconn THEN 'no connections allowed' END AS state, "
                 "  shobj_description(d.oid, 'pg_database') AS comment "
                 "FROM pg_database d LEFT JOIN pg_tablespace t ON t.oid = d.dattablespace "
                 "WHERE d.oid = "
                     + db},
                {"Settings",
                 "SELECT r.rolname AS role, unnest(s.setconfig) AS setting "
                 "FROM pg_db_role_setting s LEFT JOIN pg_roles r ON r.oid = s.setrole "
                 "WHERE s.setdatabase = "
                     + db + " ORDER BY 1, 2",
                 false, true}};
    }

    case Schema:
        return {
            {nullptr,
             "SELECT n.nspname AS name, pg_get_userbyid(n.nspowner) AS owner, "
             "  array_to_string(n.nspacl, E'\\n') AS privileges, "
             "  obj_description(n.oid, 'pg_namespace') AS comment "
             "FROM pg_namespace n WHERE n.oid = "
                 + id},
            {"Contents",
             "SELECT kind, count(*)::text AS count FROM ("
             "  SELECT CASE c.relkind WHEN 'r' THEN 'tables' WHEN 'p' THEN 'partitioned tables' "
             "    WHEN 'v' THEN 'views' WHEN 'm' THEN 'materialized views' "
             "    WHEN 'f' THEN 'foreign tables' WHEN 'S' THEN 'sequences' "
             "    WHEN 'i' THEN 'indexes' ELSE c.relkind::text END AS kind "
             "  FROM pg_class c WHERE c.relnamespace = "
                 + id
                 + " AND c.relkind <> 't' "
                   "  UNION ALL SELECT CASE p.prokind WHEN 'p' THEN 'procedures' "
                   "    WHEN 'a' THEN 'aggregates' ELSE 'functions' END "
                   "  FROM pg_proc p WHERE p.pronamespace = "
                 + id
                 + "  UNION ALL SELECT CASE t.typtype WHEN 'd' THEN 'domains' ELSE 'types' END "
                   "  FROM pg_type t WHERE t.typnamespace = "
                 + id
                 + "    AND (t.typrelid = 0 OR (SELECT c.relkind FROM pg_class c "
                   "      WHERE c.oid = t.typrelid) = 'c') AND t.typelem = 0"
                   ") s GROUP BY 1 ORDER BY 1"}};

    case Sequence:
        return {
            {nullptr,
             "SELECT c.relname AS name, n.nspname AS schema, pg_get_userbyid(c.relowner) AS owner, "
             "  format_type(s.seqtypid, NULL) AS type, s.seqstart::text AS start, "
             "  s.seqincrement::text AS increment, s.seqmin::text AS minimum, "
             "  s.seqmax::text AS maximum, s.seqcache::text AS cache, "
             "  CASE WHEN s.seqcycle THEN 'yes' ELSE 'no' END AS cycles, "
             "  pg_sequence_last_value(c.oid)::text AS \"last value\", "
             "  (SELECT dc.relname || '.' || a.attname FROM pg_depend d "
             "     JOIN pg_class dc ON dc.oid = d.refobjid "
             "     JOIN pg_attribute a ON a.attrelid = d.refobjid AND a.attnum = d.refobjsubid "
             "   WHERE d.objid = c.oid AND d.classid = 'pg_class'::regclass "
             "     AND d.refclassid = 'pg_class'::regclass AND d.deptype IN ('a', 'i')) AS \"owned "
             "by\", "
             "  obj_description(c.oid, 'pg_class') AS comment "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "JOIN pg_sequence s ON s.seqrelid = c.oid WHERE c.oid = "
                 + id}};

    case Function:
    case Procedure:
    case Aggregate:
        return {
            {nullptr,
             "SELECT p.proname AS name, n.nspname AS schema, pg_get_userbyid(p.proowner) AS owner, "
             "  l.lanname AS language, pg_get_function_arguments(p.oid) AS arguments, "
             "  CASE WHEN p.prokind <> 'p' THEN pg_get_function_result(p.oid) END AS returns, "
             "  CASE p.provolatile WHEN 'i' THEN 'immutable' WHEN 's' THEN 'stable' "
             "    ELSE 'volatile' END AS volatility, "
             "  CASE p.proparallel WHEN 's' THEN 'safe' WHEN 'r' THEN 'restricted' "
             "    ELSE 'unsafe' END AS parallel, "
             "  CASE WHEN p.prosecdef THEN 'definer' ELSE 'invoker' END AS \"security\", "
             "  CASE WHEN p.proisstrict THEN 'strict' END AS \"on null input\", "
             "  p.procost::text AS cost, obj_description(p.oid, 'pg_proc') AS comment "
             "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
             "LEFT JOIN pg_language l ON l.oid = p.prolang WHERE p.oid = "
                 + id},
            // An aggregate has no source to show; it has its support functions.
            {kind == Aggregate ? "Support" : nullptr,
             kind == Aggregate
                 ? "SELECT 'transition function' AS part, a.aggtransfn::regproc::text AS value "
                   "FROM pg_aggregate a WHERE a.aggfnoid = "
                     + id
                     + " UNION ALL SELECT 'state type', format_type(a.aggtranstype, NULL) "
                       "FROM pg_aggregate a WHERE a.aggfnoid = "
                     + id
                     + " UNION ALL SELECT 'final function', a.aggfinalfn::regproc::text "
                       "FROM pg_aggregate a WHERE a.aggfnoid = "
                     + id + " AND a.aggfinalfn <> 0"
                 : "SELECT pg_get_functiondef(" + id + ")",
             kind != Aggregate, kind == Aggregate}};

    case Type:
    case Domain:
        return {
            {nullptr,
             "SELECT t.typname AS name, n.nspname AS schema, pg_get_userbyid(t.typowner) AS owner, "
             "  CASE t.typtype WHEN 'b' THEN 'base' WHEN 'c' THEN 'composite' "
             "    WHEN 'd' THEN 'domain' WHEN 'e' THEN 'enum' WHEN 'r' THEN 'range' "
             "    WHEN 'm' THEN 'multirange' WHEN 'p' THEN 'pseudo' END AS type, "
             "  CASE WHEN t.typtype = 'd' THEN format_type(t.typbasetype, t.typtypmod) END AS "
             "\"base type\", "
             "  CASE WHEN t.typtype = 'd' AND t.typnotnull THEN 'not null' END AS nullable, "
             "  t.typdefault AS default, "
             "  CASE WHEN t.typtype = 'r' THEN (SELECT format_type(r.rngsubtype, NULL) "
             "    FROM pg_range r WHERE r.rngtypid = t.oid) END AS \"subtype\", "
             "  obj_description(t.oid, 'pg_type') AS comment "
             "FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace WHERE t.oid = "
                 + id},
            {"Values",
             "SELECT e.enumlabel AS label FROM pg_enum e WHERE e.enumtypid = " + id
                 + " ORDER BY e.enumsortorder",
             false, true},
            {"Attributes",
             "SELECT a.attnum AS \"#\", a.attname AS name, "
             "  format_type(a.atttypid, a.atttypmod) AS type "
             "FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid "
             "WHERE NOT a.attisdropped AND a.attnum > 0 AND c.reltype = "
                 + id + " ORDER BY 1",
             false, true},
            {"Constraints",
             "SELECT c.conname AS name, pg_get_constraintdef(c.oid) AS definition "
             "FROM pg_constraint c WHERE c.contypid = "
                 + id + " ORDER BY 1",
             false, true}};

    case Index:
        return {
            {nullptr,
             "SELECT ic.relname AS name, n.nspname AS schema, "
             "  pg_get_userbyid(ic.relowner) AS owner, tc.relname AS table, "
             "  am.amname AS method, "
             "  CASE WHEN i.indisprimary THEN 'primary key' WHEN i.indisunique THEN 'unique' "
             "    ELSE 'non-unique' END AS kind, "
             "  CASE WHEN NOT i.indisvalid THEN 'invalid' END AS state, "
             "  ts.spcname AS tablespace, "
             "  replace(pg_size_pretty(pg_relation_size(ic.oid)), ' bytes', ' B') AS size, "
             "  obj_description(ic.oid, 'pg_class') AS comment "
             "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
             "JOIN pg_class tc ON tc.oid = i.indrelid "
             "JOIN pg_namespace n ON n.oid = ic.relnamespace "
             "LEFT JOIN pg_am am ON am.oid = ic.relam "
             "LEFT JOIN pg_tablespace ts ON ts.oid = ic.reltablespace WHERE ic.oid = "
                 + id},
            {"Columns",
             "SELECT a.attnum AS \"#\", pg_get_indexdef(a.attrelid, a.attnum, true) AS expression "
             "FROM pg_attribute a WHERE a.attrelid = "
                 + id + " ORDER BY 1"},
            {nullptr, "SELECT pg_get_indexdef(" + id + ")", true}};

    case Trigger:
        return {{nullptr,
                 "SELECT t.tgname AS name, c.relname AS table, n.nspname AS schema, "
                 "  p.proname AS function, "
                 "  CASE t.tgenabled WHEN 'D' THEN 'disabled' WHEN 'O' THEN 'enabled' "
                 "    WHEN 'R' THEN 'enabled (replica)' WHEN 'A' THEN 'enabled (always)' END AS "
                 "status, "
                 "  CASE WHEN (t.tgtype & 1) = 1 THEN 'row' ELSE 'statement' END AS \"for each\", "
                 "  CASE WHEN (t.tgtype & 2) = 2 THEN 'before' WHEN (t.tgtype & 64) = 64 "
                 "    THEN 'instead of' ELSE 'after' END AS timing, "
                 "  concat_ws(', ', CASE WHEN (t.tgtype & 4) = 4 THEN 'insert' END, "
                 "    CASE WHEN (t.tgtype & 8) = 8 THEN 'delete' END, "
                 "    CASE WHEN (t.tgtype & 16) = 16 THEN 'update' END, "
                 "    CASE WHEN (t.tgtype & 32) = 32 THEN 'truncate' END) AS events, "
                 "  obj_description(t.oid, 'pg_trigger') AS comment "
                 "FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid "
                 "JOIN pg_namespace n ON n.oid = c.relnamespace "
                 "JOIN pg_proc p ON p.oid = t.tgfoid WHERE t.oid = "
                     + id},
                {nullptr, "SELECT pg_get_triggerdef(" + id + ", true)", true}};

    case Policy:
        return {{nullptr,
                 "SELECT p.polname AS name, c.relname AS table, n.nspname AS schema, "
                 "  CASE p.polcmd WHEN 'r' THEN 'select' WHEN 'a' THEN 'insert' "
                 "    WHEN 'w' THEN 'update' WHEN 'd' THEN 'delete' ELSE 'all' END AS command, "
                 "  CASE WHEN p.polpermissive THEN 'permissive' ELSE 'restrictive' END AS kind, "
                 "  CASE WHEN p.polroles = '{0}' THEN 'public' "
                 "    ELSE (SELECT string_agg(r.rolname, ', ' ORDER BY r.rolname) FROM pg_roles r "
                 "          WHERE r.oid = ANY (p.polroles)) END AS roles, "
                 "  pg_get_expr(p.polqual, p.polrelid) AS using, "
                 "  pg_get_expr(p.polwithcheck, p.polrelid) AS \"with check\" "
                 "FROM pg_policy p JOIN pg_class c ON c.oid = p.polrelid "
                 "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE p.oid = "
                     + id}};

    case Constraint:
        return {{nullptr,
                 "SELECT c.conname AS name, tc.relname AS table, n.nspname AS schema, "
                 "  CASE c.contype WHEN 'p' THEN 'primary key' WHEN 'f' THEN 'foreign key' "
                 "    WHEN 'u' THEN 'unique' WHEN 'c' THEN 'check' WHEN 'x' THEN 'exclude' "
                 "    WHEN 'n' THEN 'not null' WHEN 't' THEN 'trigger' END AS type, "
                 "  CASE WHEN c.condeferrable THEN 'yes' ELSE 'no' END AS deferrable, "
                 "  CASE WHEN c.convalidated THEN 'yes' ELSE 'no' END AS validated, "
                 "  ftc.relname AS references, pg_get_constraintdef(c.oid) AS definition, "
                 "  obj_description(c.oid, 'pg_constraint') AS comment "
                 "FROM pg_constraint c LEFT JOIN pg_class tc ON tc.oid = c.conrelid "
                 "LEFT JOIN pg_class ftc ON ftc.oid = c.confrelid "
                 "LEFT JOIN pg_namespace n ON n.oid = c.connamespace WHERE c.oid = "
                     + id}};

    case Extension:
        return {
            {nullptr,
             "SELECT e.extname AS name, e.extversion AS version, n.nspname AS schema, "
             "  pg_get_userbyid(e.extowner) AS owner, "
             "  CASE WHEN e.extrelocatable THEN 'yes' ELSE 'no' END AS relocatable, "
             "  a.default_version AS \"default version\", a.comment AS description "
             "FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace "
             "LEFT JOIN pg_available_extensions a ON a.name = e.extname WHERE e.oid = "
                 + id},
            {"Objects",
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
                 + id + " ORDER BY 1, 2"}};

    case Publication:
        return {
            {nullptr,
             "SELECT p.pubname AS name, pg_get_userbyid(p.pubowner) AS owner, "
             "  CASE WHEN p.puballtables THEN 'all tables' END AS scope, "
             "  concat_ws(', ', CASE WHEN p.pubinsert THEN 'insert' END, "
             "    CASE WHEN p.pubupdate THEN 'update' END, "
             "    CASE WHEN p.pubdelete THEN 'delete' END, "
             "    CASE WHEN p.pubtruncate THEN 'truncate' END) AS publishes, "
             "  CASE WHEN p.pubviaroot THEN 'yes' ELSE 'no' END AS \"via root\" "
             "FROM pg_publication p WHERE p.oid = "
                 + id},
            {"Tables",
             "SELECT n.nspname AS schema, c.relname AS name FROM pg_publication_rel r "
             "JOIN pg_class c ON c.oid = r.prrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE r.prpubid = "
                 + id + " ORDER BY 1, 2"}};

    case EventTrigger:
        return {{nullptr,
                 "SELECT e.evtname AS name, pg_get_userbyid(e.evtowner) AS owner, "
                 "  e.evtevent AS event, p.proname AS function, "
                 "  CASE e.evtenabled WHEN 'D' THEN 'disabled' WHEN 'O' THEN 'enabled' "
                 "    WHEN 'R' THEN 'enabled (replica)' ELSE 'enabled (always)' END AS status, "
                 "  array_to_string(e.evttags, ', ') AS tags, "
                 "  obj_description(e.oid, 'pg_event_trigger') AS comment "
                 "FROM pg_event_trigger e JOIN pg_proc p ON p.oid = e.evtfoid WHERE e.oid = "
                     + id}};

    case Role:
        return {{nullptr,
                 "SELECT r.rolname AS name, "
                 "  concat_ws(', ', CASE WHEN r.rolsuper THEN 'superuser' END, "
                 "    CASE WHEN r.rolcanlogin THEN 'login' END, "
                 "    CASE WHEN r.rolcreatedb THEN 'create databases' END, "
                 "    CASE WHEN r.rolcreaterole THEN 'create roles' END, "
                 "    CASE WHEN r.rolreplication THEN 'replication' END, "
                 "    CASE WHEN r.rolbypassrls THEN 'bypass row security' END) AS attributes, "
                 "  CASE WHEN r.rolinherit THEN 'yes' ELSE 'no' END AS inherits, "
                 "  CASE WHEN r.rolconnlimit >= 0 THEN r.rolconnlimit::text END AS \"connection "
                 "limit\", "
                 "  r.rolvaliduntil::text AS \"valid until\", "
                 "  shobj_description(r.oid, 'pg_authid') AS comment "
                 "FROM pg_roles r WHERE r.oid = "
                     + id},
                {"Member of",
                 "SELECT g.rolname AS role, "
                 "  CASE WHEN m.admin_option THEN 'with admin option' END AS options "
                 "FROM pg_auth_members m JOIN pg_roles g ON g.oid = m.roleid "
                 "WHERE m.member = "
                     + id + " ORDER BY 1",
                 false, true},
                {"Members",
                 "SELECT r.rolname AS role, "
                 "  CASE WHEN m.admin_option THEN 'with admin option' END AS options "
                 "FROM pg_auth_members m JOIN pg_roles r ON r.oid = m.member "
                 "WHERE m.roleid = "
                     + id + " ORDER BY 1",
                 false, true}};

    case Tablespace:
        return {{nullptr,
                 "SELECT t.spcname AS name, pg_get_userbyid(t.spcowner) AS owner, "
                 "  pg_tablespace_location(t.oid) AS location, "
                 "  replace(pg_size_pretty(pg_tablespace_size(t.oid)), ' bytes', ' B') AS size, "
                 "  array_to_string(t.spcoptions, ', ') AS options, "
                 "  shobj_description(t.oid, 'pg_tablespace') AS comment "
                 "FROM pg_tablespace t WHERE t.oid = "
                     + id},
                {"Databases",
                 "SELECT d.datname AS name FROM pg_database d WHERE d.dattablespace = " + id
                     + " ORDER BY 1",
                 false, true}};

    case Column:
        return {}; // Its table's Columns tab says everything there is.
    }
    return {};
}

} // namespace

bool hasDetails(ObjectKind kind)
{
    return !sectionsOf(kind, 0).empty();
}

std::vector<QByteArray> detailQueries(ObjectKind kind, Oid oid)
{
    std::vector<QByteArray> queries;
    for (const Section &section : sectionsOf(kind, oid))
        queries.push_back(section.sql);
    return queries;
}

ObjectDetail parseDetail(ObjectKind kind, const std::vector<pg::Result> &results)
{
    ObjectDetail detail;
    detail.subtitle = kindName(kind);
    if (results.empty() || results[0].rowCount() == 0)
        return detail;

    QString name;
    QString schema;
    detail.properties = propertiesOf(results[0], &name, &schema);
    // An extension's, trigger's, policy's or constraint's schema says where
    // it lives, but its name is not qualified by it.
    const bool qualified = kind != ObjectKind::Extension && kind != ObjectKind::Trigger
        && kind != ObjectKind::Policy && kind != ObjectKind::Constraint;
    detail.title = schema.isEmpty() || !qualified ? name : schema + QLatin1Char('.') + name;

    const std::vector<Section> sections = sectionsOf(kind, 0);
    for (std::size_t i = 1; i < sections.size() && i < results.size(); ++i) {
        const Section &section = sections[i];
        const pg::Result &result = results[i];
        if (section.definition) {
            if (result.rowCount() > 0)
                detail.definition = text(result, 0, 0);
            continue;
        }
        if (section.hideWhenEmpty && result.rowCount() == 0)
            continue;
        detail.tables.push_back(
            tableOf(QCoreApplication::translate("slonisko::catalog", section.title), result));
    }
    return detail;
}

} // namespace slonisko::catalog
