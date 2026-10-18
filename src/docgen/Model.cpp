// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Model.h"

#include "pg/Result.h"
#include "sql/Keywords.h"

#include <QHash>

#include <algorithm>

namespace slonisko::docgen {

namespace {

enum Query {
    DatabaseQuery,
    SchemasQuery,
    RelationsQuery,
    ColumnsQuery,
    ConstraintsQuery,
    IndexesQuery,
    TriggersQuery,
    PoliciesQuery,
    DependenciesQuery,
    SequencesQuery,
    RoutinesQuery,
    TypesQuery,
    TypeAttributesQuery,
    DomainConstraintsQuery,
    ExtensionsQuery,
    QueryCount
};

QByteArray literal(const QString &text)
{
    QByteArray quoted = text.toUtf8();
    quoted.replace('\'', "''");
    return '\'' + quoted + '\'';
}

// A shell-like pattern as a LIKE pattern: * and ? are the wildcards, and
// LIKE's own are taken literally.
QByteArray likePattern(const QString &pattern)
{
    QString like;
    for (const QChar c : pattern) {
        if (c == QLatin1Char('*'))
            like += QLatin1Char('%');
        else if (c == QLatin1Char('?'))
            like += QLatin1Char('_');
        else if (c == QLatin1Char('%') || c == QLatin1Char('_') || c == QLatin1Char('\\'))
            like += QLatin1Char('\\') + c;
        else
            like += c;
    }
    return literal(like);
}

QByteArray patternArray(const QStringList &patterns)
{
    QByteArray array = "ARRAY[";
    for (qsizetype i = 0; i < patterns.size(); ++i) {
        if (i > 0)
            array += ", ";
        array += likePattern(patterns[i]);
    }
    return array + "]::text[]";
}

QByteArray notInExtension(const char *catalog, const char *oid)
{
    return QByteArray("NOT EXISTS (SELECT 1 FROM pg_depend e WHERE e.classid = '") + catalog
        + "'::regclass AND e.objid = " + oid + " AND e.deptype = 'e')";
}

QString text(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QString() : QString::fromUtf8(result.value(row, column));
}

bool flag(const pg::Result &result, int row, int column)
{
    return result.value(row, column) == "t";
}

qint64 integer(const pg::Result &result, int row, int column)
{
    return result.value(row, column).toLongLong();
}

Oid oid(const pg::Result &result, int row, int column)
{
    return result.value(row, column).toUInt();
}

char character(const pg::Result &result, int row, int column)
{
    const QByteArray value = result.value(row, column);
    return value.isEmpty() ? '\0' : value.front();
}

QStringList array(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QStringList()
                                      : catalog::parseTextArray(text(result, row, column));
}

QString referentialAction(char action)
{
    switch (action) {
    case 'r':
        return QStringLiteral("RESTRICT");
    case 'c':
        return QStringLiteral("CASCADE");
    case 'n':
        return QStringLiteral("SET NULL");
    case 'd':
        return QStringLiteral("SET DEFAULT");
    default:
        return {};
    }
}

// The columns query and the composite types' attributes query share a layout.
Column parseColumn(const pg::Result &result, int row)
{
    Column column;
    column.number = int(integer(result, row, 1));
    column.name = text(result, row, 2);
    column.type = text(result, row, 3);
    column.notNull = flag(result, row, 4);
    column.defaultValue = text(result, row, 5);
    const char identity = character(result, row, 6);
    if (identity == 'a')
        column.identity = QStringLiteral("ALWAYS");
    else if (identity == 'd')
        column.identity = QStringLiteral("BY DEFAULT");
    column.generated = text(result, row, 7);
    column.comment = text(result, row, 8);
    column.virtualGenerated = character(result, row, 9) == 'v';
    return column;
}

// So do the table and the domain constraints queries.
Constraint parseConstraint(const pg::Result &result, int row)
{
    Constraint constraint;
    constraint.name = text(result, row, 1);
    constraint.type = character(result, row, 2);
    constraint.columns = array(result, row, 3);
    constraint.definition = text(result, row, 4);
    constraint.comment = text(result, row, 5);
    constraint.referencedOid = oid(result, row, 6);
    constraint.referencedSchema = text(result, row, 7);
    constraint.referencedTable = text(result, row, 8);
    constraint.referencedColumns = array(result, row, 9);
    constraint.onUpdate = referentialAction(character(result, row, 10));
    constraint.onDelete = referentialAction(character(result, row, 11));
    return constraint;
}

} // namespace

QByteArray schemaCondition(const SchemaFilter &filter)
{
    QByteArray condition = "n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'";
    if (!filter.include.isEmpty())
        condition += " AND n.nspname LIKE ANY (" + patternArray(filter.include) + ")";
    if (!filter.exclude.isEmpty())
        condition += " AND NOT n.nspname LIKE ANY (" + patternArray(filter.exclude) + ")";
    return condition;
}

const Column *Relation::column(const QString &columnName) const
{
    const auto it = std::ranges::find(columns, columnName, &Column::name);
    return it == columns.end() ? nullptr : &*it;
}

const Constraint *Relation::primaryKey() const
{
    const auto it = std::ranges::find(constraints, 'p', &Constraint::type);
    return it == constraints.end() ? nullptr : &*it;
}

const Relation *Model::relation(Oid relationOid) const
{
    const auto it = std::ranges::find(relations, relationOid, &Relation::oid);
    return it == relations.end() ? nullptr : &*it;
}

const Relation *Model::relation(const QString &schema, const QString &name) const
{
    const auto it = std::ranges::find_if(
        relations, [&](const Relation &r) { return r.schema == schema && r.name == name; });
    return it == relations.end() ? nullptr : &*it;
}

std::vector<QByteArray> modelQueries(const SchemaFilter &filter)
{
    const QByteArray schemas = schemaCondition(filter);
    // The relations documented, which the queries about their parts start from.
    const QByteArray rels = "WITH rels AS (SELECT c.oid FROM pg_class c "
                            "JOIN pg_namespace n ON n.oid = c.relnamespace "
                            "WHERE c.relkind IN ('r', 'p', 'f', 'v', 'm') AND "
        + schemas + " AND " + notInExtension("pg_class", "c.oid") + ") ";
    // Column names of a key, in the order the key has them.
    const auto keyColumns = [](const char *key, const char *relation) {
        return QByteArray("ARRAY(SELECT a.attname FROM unnest(c.") + key
            + ") WITH ORDINALITY k(n, o) JOIN pg_attribute a ON a.attrelid = c." + relation
            + " AND a.attnum = k.n ORDER BY k.o)";
    };

    std::vector<QByteArray> queries(QueryCount);
    queries[DatabaseQuery] = "SELECT current_database(), current_setting('server_version'), "
                             "pg_get_userbyid(d.datdba), pg_encoding_to_char(d.encoding), "
                             "shobj_description(d.oid, 'pg_database') "
                             "FROM pg_database d WHERE d.datname = current_database()";
    queries[SchemasQuery] = "SELECT n.nspname, pg_get_userbyid(n.nspowner), "
                            "obj_description(n.oid, 'pg_namespace') "
                            "FROM pg_namespace n WHERE "
        + schemas + " ORDER BY n.nspname";
    queries[RelationsQuery] = rels
        + "SELECT c.oid, n.nspname, c.relname, c.relkind, pg_get_userbyid(c.relowner), "
          "obj_description(c.oid, 'pg_class'), c.reltuples, "
          "CASE WHEN c.relkind IN ('r', 'm') THEN pg_total_relation_size(c.oid) END, "
          "c.relpersistence = 'u', c.relrowsecurity, array_to_string(c.reloptions, ', '), "
          "ts.spcname, CASE WHEN c.relkind = 'p' THEN pg_get_partkeydef(c.oid) END, "
          "pn.nspname, p.relname, pg_get_expr(c.relpartbound, c.oid), fs.srvname, "
          "CASE WHEN c.relkind IN ('v', 'm') THEN pg_get_viewdef(c.oid, true) END "
          "FROM rels JOIN pg_class c ON c.oid = rels.oid "
          "JOIN pg_namespace n ON n.oid = c.relnamespace "
          "LEFT JOIN pg_tablespace ts ON ts.oid = c.reltablespace "
          "LEFT JOIN pg_inherits i ON i.inhrelid = c.oid AND c.relispartition "
          "LEFT JOIN pg_class p ON p.oid = i.inhparent "
          "LEFT JOIN pg_namespace pn ON pn.oid = p.relnamespace "
          "LEFT JOIN pg_foreign_table ft ON ft.ftrelid = c.oid "
          "LEFT JOIN pg_foreign_server fs ON fs.oid = ft.ftserver "
          "ORDER BY n.nspname, c.relname";
    queries[ColumnsQuery] = rels
        + "SELECT a.attrelid, a.attnum, a.attname, format_type(a.atttypid, a.atttypmod), "
          "a.attnotnull, CASE WHEN a.attgenerated = '' THEN pg_get_expr(d.adbin, d.adrelid) END, "
          "a.attidentity, "
          "CASE WHEN a.attgenerated <> '' THEN pg_get_expr(d.adbin, d.adrelid) END, "
          "col_description(a.attrelid, a.attnum), a.attgenerated "
          "FROM rels JOIN pg_attribute a ON a.attrelid = rels.oid "
          "AND a.attnum > 0 AND NOT a.attisdropped "
          "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
          "ORDER BY a.attrelid, a.attnum";
    // A foreign key to a partitioned table has a copy for each partition on
    // the same table; only the one the user made is listed. NOT NULL
    // constraints, catalogued since 18, are already in the columns.
    queries[ConstraintsQuery] = rels + "SELECT c.conrelid, c.conname, c.contype, "
        + keyColumns("conkey", "conrelid")
        + ", pg_get_constraintdef(c.oid, true), obj_description(c.oid, 'pg_constraint'), "
          "c.confrelid, fn.nspname, f.relname, "
        + keyColumns("confkey", "confrelid")
        + ", c.confupdtype, c.confdeltype "
          "FROM rels JOIN pg_constraint c ON c.conrelid = rels.oid "
          "LEFT JOIN pg_class f ON f.oid = c.confrelid "
          "LEFT JOIN pg_namespace fn ON fn.oid = f.relnamespace "
          "WHERE c.contype <> 'n' AND NOT EXISTS (SELECT 1 FROM pg_constraint pc "
          "WHERE pc.oid = c.conparentid AND pc.conrelid = c.conrelid) "
          "ORDER BY c.conrelid, c.contype, c.conname";
    queries[IndexesQuery] = rels
        + "SELECT i.indrelid, ic.relname, am.amname, "
          "ARRAY(SELECT pg_get_indexdef(i.indexrelid, k, true) "
          "FROM generate_series(1, i.indnatts) k ORDER BY k), "
          "i.indnkeyatts, i.indisunique, i.indisprimary, i.indisvalid, i.indpred IS NOT NULL, "
          "pg_get_indexdef(i.indexrelid), pg_relation_size(i.indexrelid), "
          "obj_description(i.indexrelid, 'pg_class'), "
          // int2vector and oidvector count from 0.
          "ARRAY(SELECT i.indoption[k] FROM generate_series(0, i.indnkeyatts - 1) k ORDER BY k), "
          "ARRAY(SELECT concat_ws(' ', CASE WHEN NOT o.opcdefault THEN o.opcname END, "
          "CASE WHEN i.indcollation[k] <> 0 "
          "AND i.indcollation[k] <> coalesce(a.attcollation, 100) "
          "THEN 'COLLATE ' || co.collname END) "
          "FROM generate_series(0, i.indnkeyatts - 1) k "
          "JOIN pg_opclass o ON o.oid = i.indclass[k] "
          "LEFT JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = i.indkey[k] "
          "LEFT JOIN pg_collation co ON co.oid = i.indcollation[k] ORDER BY k) "
          "FROM rels JOIN pg_index i ON i.indrelid = rels.oid "
          "JOIN pg_class ic ON ic.oid = i.indexrelid "
          "JOIN pg_am am ON am.oid = ic.relam "
          "ORDER BY i.indrelid, ic.relname";
    queries[TriggersQuery] = rels
        + "SELECT t.tgrelid, t.tgname, pn.nspname || '.' || p.proname, t.tgenabled <> 'D', "
          "pg_get_triggerdef(t.oid, true), obj_description(t.oid, 'pg_trigger') "
          "FROM rels JOIN pg_trigger t ON t.tgrelid = rels.oid "
          "JOIN pg_proc p ON p.oid = t.tgfoid "
          "JOIN pg_namespace pn ON pn.oid = p.pronamespace "
          "WHERE NOT t.tgisinternal "
          "ORDER BY t.tgrelid, t.tgname";
    queries[PoliciesQuery] = rels
        + "SELECT p.polrelid, p.polname, CASE p.polcmd WHEN 'r' THEN 'SELECT' "
          "WHEN 'a' THEN 'INSERT' WHEN 'w' THEN 'UPDATE' WHEN 'd' THEN 'DELETE' ELSE 'ALL' END, "
          "p.polpermissive, ARRAY(SELECT CASE WHEN r = 0 THEN 'public' "
          "ELSE pg_get_userbyid(r) END FROM unnest(p.polroles) r), "
          "pg_get_expr(p.polqual, p.polrelid), pg_get_expr(p.polwithcheck, p.polrelid) "
          "FROM rels JOIN pg_policy p ON p.polrelid = rels.oid "
          "ORDER BY p.polrelid, p.polname";
    queries[DependenciesQuery] = rels
        + "SELECT DISTINCT r.ev_class, d.refobjid "
          "FROM rels JOIN pg_rewrite r ON r.ev_class = rels.oid "
          "JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
          "AND d.refclassid = 'pg_class'::regclass AND d.refobjid <> r.ev_class "
          "ORDER BY 1, 2";
    // A serial or identity column owns its sequence.
    queries[SequencesQuery]
        = "SELECT n.nspname, c.relname, format_type(s.seqtypid, NULL), s.seqstart, "
          "s.seqincrement, s.seqmin, s.seqmax, s.seqcache, s.seqcycle, "
          "(SELECT tn.nspname || '.' || t.relname || '.' || a.attname FROM pg_depend d "
          "JOIN pg_class t ON t.oid = d.refobjid "
          "JOIN pg_namespace tn ON tn.oid = t.relnamespace "
          "JOIN pg_attribute a ON a.attrelid = d.refobjid AND a.attnum = d.refobjsubid "
          "WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid "
          "AND d.refclassid = 'pg_class'::regclass AND d.deptype IN ('a', 'i') LIMIT 1), "
          "pg_get_userbyid(c.relowner), obj_description(c.oid, 'pg_class') "
          "FROM pg_sequence s JOIN pg_class c ON c.oid = s.seqrelid "
          "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE "
        + schemas + " AND " + notInExtension("pg_class", "c.oid") + " ORDER BY 1, 2";
    // pg_get_functiondef() refuses aggregates; CASE keeps it from seeing one.
    queries[RoutinesQuery]
        = "SELECT p.oid, n.nspname, p.proname, p.prokind, "
          "pg_get_function_identity_arguments(p.oid), pg_get_function_result(p.oid), "
          "l.lanname, p.provolatile, p.prosecdef, pg_get_userbyid(p.proowner), "
          "obj_description(p.oid, 'pg_proc'), "
          "CASE WHEN p.prokind <> 'a' THEN pg_get_functiondef(p.oid) END, p.prosrc "
          "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
          "JOIN pg_language l ON l.oid = p.prolang WHERE "
        + schemas + " AND " + notInExtension("pg_proc", "p.oid")
        + " ORDER BY n.nspname, p.proname, 5";
    // A table's row type is the table's; only free-standing composites count.
    const QByteArray types = "FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
                             "WHERE "
        + schemas + " AND " + notInExtension("pg_type", "t.oid");
    queries[TypesQuery]
        = "SELECT t.oid, n.nspname, t.typname, t.typtype, pg_get_userbyid(t.typowner), "
          "obj_description(t.oid, 'pg_type'), "
          "ARRAY(SELECT e.enumlabel FROM pg_enum e WHERE e.enumtypid = t.oid "
          "ORDER BY e.enumsortorder), "
          "CASE t.typtype WHEN 'd' THEN format_type(t.typbasetype, t.typtypmod) "
          "WHEN 'r' THEN (SELECT format_type(r.rngsubtype, NULL) FROM pg_range r "
          "WHERE r.rngtypid = t.oid) END, "
          "t.typnotnull, t.typdefault "
        + types
        + " AND t.typtype IN ('e', 'c', 'd', 'r') AND (t.typtype <> 'c' OR EXISTS "
          "(SELECT 1 FROM pg_class c WHERE c.oid = t.typrelid AND c.relkind = 'c')) "
          "ORDER BY 2, 3";
    queries[TypeAttributesQuery]
        = "SELECT t.oid, a.attnum, a.attname, format_type(a.atttypid, a.atttypmod), false, "
          "NULL, '', NULL, col_description(a.attrelid, a.attnum), '' "
          "FROM pg_attribute a JOIN pg_type t ON t.typrelid = a.attrelid "
          "JOIN pg_namespace n ON n.oid = t.typnamespace "
          "JOIN pg_class c ON c.oid = t.typrelid AND c.relkind = 'c' "
          "WHERE a.attnum > 0 AND NOT a.attisdropped AND "
        + schemas + " AND " + notInExtension("pg_type", "t.oid") + " ORDER BY 1, 2";
    queries[DomainConstraintsQuery]
        = "SELECT c.contypid, c.conname, c.contype, '{}'::name[], "
          "pg_get_constraintdef(c.oid, true), obj_description(c.oid, 'pg_constraint'), "
          "0, NULL, NULL, '{}'::name[], NULL, NULL "
          "FROM pg_constraint c JOIN pg_type t ON t.oid = c.contypid "
          "JOIN pg_namespace n ON n.oid = t.typnamespace "
          "WHERE c.contype <> 'n' AND "
        + schemas + " ORDER BY c.contypid, c.conname";
    queries[ExtensionsQuery] = "SELECT e.extname, e.extversion, n.nspname, "
                               "obj_description(e.oid, 'pg_extension') "
                               "FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace "
                               "ORDER BY e.extname";
    return queries;
}

Model parseModel(const std::vector<pg::Result> &results)
{
    Model model;
    if (results.size() != QueryCount)
        return model;
    for (const pg::Result &result : results) {
        if (result.isNull() || result.status() != PGRES_TUPLES_OK)
            return model;
    }

    const pg::Result &database = results[DatabaseQuery];
    if (database.rowCount() > 0) {
        model.database = text(database, 0, 0);
        model.serverVersion = text(database, 0, 1);
        model.owner = text(database, 0, 2);
        model.encoding = text(database, 0, 3);
        model.comment = text(database, 0, 4);
    }

    const pg::Result &schemas = results[SchemasQuery];
    for (int row = 0; row < schemas.rowCount(); ++row)
        model.schemas.push_back(
            {text(schemas, row, 0), text(schemas, row, 1), text(schemas, row, 2)});

    QHash<Oid, std::size_t> relations;
    const pg::Result &rels = results[RelationsQuery];
    for (int row = 0; row < rels.rowCount(); ++row) {
        Relation relation;
        relation.oid = oid(rels, row, 0);
        relation.schema = text(rels, row, 1);
        relation.name = text(rels, row, 2);
        relation.kind = character(rels, row, 3);
        relation.owner = text(rels, row, 4);
        relation.comment = text(rels, row, 5);
        relation.rows = rels.value(row, 6).toDouble();
        relation.size = integer(rels, row, 7);
        relation.unlogged = flag(rels, row, 8);
        relation.rowSecurity = flag(rels, row, 9);
        relation.options = text(rels, row, 10);
        relation.tablespace = text(rels, row, 11);
        relation.partitionKey = text(rels, row, 12);
        relation.parentSchema = text(rels, row, 13);
        relation.parentName = text(rels, row, 14);
        relation.partitionBound = text(rels, row, 15);
        relation.foreignServer = text(rels, row, 16);
        relation.definition = text(rels, row, 17).trimmed();
        relations.insert(relation.oid, model.relations.size());
        model.relations.push_back(std::move(relation));
    }
    // The parts of relations, each query ordered by the relation's oid.
    const auto owner = [&](const pg::Result &result, int row) -> Relation * {
        const auto it = relations.constFind(oid(result, row, 0));
        return it == relations.cend() ? nullptr : &model.relations[*it];
    };

    const pg::Result &columns = results[ColumnsQuery];
    for (int row = 0; row < columns.rowCount(); ++row) {
        if (Relation *relation = owner(columns, row))
            relation->columns.push_back(parseColumn(columns, row));
    }
    const pg::Result &constraints = results[ConstraintsQuery];
    for (int row = 0; row < constraints.rowCount(); ++row) {
        if (Relation *relation = owner(constraints, row))
            relation->constraints.push_back(parseConstraint(constraints, row));
    }
    const pg::Result &indexes = results[IndexesQuery];
    for (int row = 0; row < indexes.rowCount(); ++row) {
        Relation *relation = owner(indexes, row);
        if (!relation)
            continue;
        Index index;
        index.name = text(indexes, row, 1);
        index.method = text(indexes, row, 2);
        index.columns = array(indexes, row, 3);
        index.keyColumns = int(integer(indexes, row, 4));
        index.unique = flag(indexes, row, 5);
        index.primary = flag(indexes, row, 6);
        index.valid = flag(indexes, row, 7);
        index.partial = flag(indexes, row, 8);
        index.definition = text(indexes, row, 9);
        index.size = integer(indexes, row, 10);
        index.comment = text(indexes, row, 11);
        for (const QString &option : array(indexes, row, 12))
            index.keyOptions.push_back(option.toInt());
        index.keyQualifiers = array(indexes, row, 13);
        relation->indexes.push_back(std::move(index));
    }
    const pg::Result &triggers = results[TriggersQuery];
    for (int row = 0; row < triggers.rowCount(); ++row) {
        if (Relation *relation = owner(triggers, row))
            relation->triggers.push_back({text(triggers, row, 1), text(triggers, row, 2),
                                          flag(triggers, row, 3), text(triggers, row, 4),
                                          text(triggers, row, 5)});
    }
    const pg::Result &policies = results[PoliciesQuery];
    for (int row = 0; row < policies.rowCount(); ++row) {
        if (Relation *relation = owner(policies, row))
            relation->policies.push_back({text(policies, row, 1), text(policies, row, 2),
                                          flag(policies, row, 3), array(policies, row, 4),
                                          text(policies, row, 5), text(policies, row, 6)});
    }
    const pg::Result &dependencies = results[DependenciesQuery];
    for (int row = 0; row < dependencies.rowCount(); ++row)
        model.dependencies.push_back({oid(dependencies, row, 0), oid(dependencies, row, 1)});

    const pg::Result &sequences = results[SequencesQuery];
    for (int row = 0; row < sequences.rowCount(); ++row) {
        Sequence sequence;
        sequence.schema = text(sequences, row, 0);
        sequence.name = text(sequences, row, 1);
        sequence.type = text(sequences, row, 2);
        sequence.start = integer(sequences, row, 3);
        sequence.increment = integer(sequences, row, 4);
        sequence.minimum = integer(sequences, row, 5);
        sequence.maximum = integer(sequences, row, 6);
        sequence.cache = integer(sequences, row, 7);
        sequence.cycle = flag(sequences, row, 8);
        sequence.ownedBy = text(sequences, row, 9);
        sequence.owner = text(sequences, row, 10);
        sequence.comment = text(sequences, row, 11);
        model.sequences.push_back(std::move(sequence));
    }

    const pg::Result &routines = results[RoutinesQuery];
    for (int row = 0; row < routines.rowCount(); ++row) {
        Routine routine;
        routine.oid = oid(routines, row, 0);
        routine.schema = text(routines, row, 1);
        routine.name = text(routines, row, 2);
        routine.kind = character(routines, row, 3);
        routine.arguments = text(routines, row, 4);
        routine.result = text(routines, row, 5);
        routine.language = text(routines, row, 6);
        routine.volatility = character(routines, row, 7);
        routine.securityDefiner = flag(routines, row, 8);
        routine.owner = text(routines, row, 9);
        routine.comment = text(routines, row, 10);
        routine.definition = text(routines, row, 11).trimmed();
        routine.body = text(routines, row, 12);
        model.routines.push_back(std::move(routine));
    }

    QHash<Oid, std::size_t> types;
    const pg::Result &typeRows = results[TypesQuery];
    for (int row = 0; row < typeRows.rowCount(); ++row) {
        UserType type;
        type.oid = oid(typeRows, row, 0);
        type.schema = text(typeRows, row, 1);
        type.name = text(typeRows, row, 2);
        type.kind = character(typeRows, row, 3);
        type.owner = text(typeRows, row, 4);
        type.comment = text(typeRows, row, 5);
        type.labels = array(typeRows, row, 6);
        type.baseType = text(typeRows, row, 7);
        type.notNull = flag(typeRows, row, 8);
        type.defaultValue = text(typeRows, row, 9);
        types.insert(type.oid, model.types.size());
        model.types.push_back(std::move(type));
    }
    const auto typeOf = [&](const pg::Result &result, int row) -> UserType * {
        const auto it = types.constFind(oid(result, row, 0));
        return it == types.cend() ? nullptr : &model.types[*it];
    };
    const pg::Result &attributes = results[TypeAttributesQuery];
    for (int row = 0; row < attributes.rowCount(); ++row) {
        if (UserType *type = typeOf(attributes, row))
            type->attributes.push_back(parseColumn(attributes, row));
    }
    const pg::Result &domainConstraints = results[DomainConstraintsQuery];
    for (int row = 0; row < domainConstraints.rowCount(); ++row) {
        if (UserType *type = typeOf(domainConstraints, row))
            type->constraints.push_back(parseConstraint(domainConstraints, row));
    }

    const pg::Result &extensions = results[ExtensionsQuery];
    for (int row = 0; row < extensions.rowCount(); ++row)
        model.extensions.push_back({text(extensions, row, 0), text(extensions, row, 1),
                                    text(extensions, row, 2), text(extensions, row, 3)});
    return model;
}

QString quoteIdentifier(const QString &name)
{
    if (!sql::needsQuoting(name.toUtf8()))
        return name;
    QString quoted = name;
    quoted.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString quoteQualified(const QString &schema, const QString &name)
{
    return quoteIdentifier(schema) + QLatin1Char('.') + quoteIdentifier(name);
}

QByteArray schemaListQuery()
{
    return "SELECT n.nspname FROM pg_namespace n WHERE " + schemaCondition({})
        + " ORDER BY n.nspname";
}

QStringList parseSchemaList(const pg::Result &result)
{
    QStringList names;
    for (int row = 0; row < result.rowCount(); ++row)
        names << text(result, row, 0);
    return names;
}

} // namespace slonisko::docgen
