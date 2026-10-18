// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::docgen {

using catalog::Oid;

// Everything the documentation says about a database, read from its catalog
// in one go. Plain data: nothing here talks to a server, so the pages can be
// built and tested from a model made by hand.

struct Column
{
    int number = 0; // pg_attribute.attnum
    QString name;
    QString type; // As format_type() spells it.
    bool notNull = false;
    QString defaultValue;
    QString identity; // "ALWAYS", "BY DEFAULT" or empty.
    QString generated; // The expression of a generated column.
    bool virtualGenerated = false; // Computed when read (18+) rather than stored.
    QString comment;
};

struct Constraint
{
    QString name;
    char type = 'p'; // pg_constraint.contype: p, u, f, c, x, n (NOT NULL, 18+).
    QStringList columns;
    QString definition; // pg_get_constraintdef()
    QString comment;
    // Foreign keys only.
    Oid referencedOid = 0;
    QString referencedSchema;
    QString referencedTable;
    QStringList referencedColumns;
    QString onUpdate; // "CASCADE", "SET NULL", ... empty for NO ACTION.
    QString onDelete;
};

struct Index
{
    QString name;
    QString method; // btree, gin, ...
    QStringList columns; // Key columns or expressions, as pg_get_indexdef() spells them.
    int keyColumns = 0; // The rest of columns are INCLUDEd.
    // For each key column: pg_index.indoption, 1 for DESC and 2 for NULLS
    // FIRST; and its operator class and collation where they are not the
    // default ones, "text_pattern_ops COLLATE C", or else empty.
    std::vector<int> keyOptions;
    QStringList keyQualifiers;
    bool unique = false;
    bool primary = false;
    bool valid = true;
    bool partial = false;
    QString definition;
    qint64 size = 0; // Bytes.
    QString comment;
};

struct Trigger
{
    QString name;
    QString function; // schema.name
    bool enabled = true;
    QString definition;
    QString comment;
};

struct Policy
{
    QString name;
    QString command; // ALL, SELECT, ...
    bool permissive = true;
    QStringList roles;
    QString using_;
    QString withCheck;
};

struct Relation
{
    Oid oid = 0;
    QString schema;
    QString name;
    char kind = 'r'; // pg_class.relkind: r, p, f, v, m.
    QString owner;
    QString comment;
    double rows = -1; // The planner's estimate; -1 when never analysed.
    qint64 size = 0; // Bytes, indexes and TOAST included.
    bool unlogged = false;
    bool rowSecurity = false;
    QString options; // reloptions, comma separated.
    QString tablespace;
    QString partitionKey; // Of a partitioned table: pg_get_partkeydef().
    QString parentSchema; // Of a partition.
    QString parentName;
    QString partitionBound;
    QString foreignServer;
    QString definition; // Of a view or a materialized view.
    std::vector<Column> columns;
    std::vector<Constraint> constraints;
    std::vector<Index> indexes;
    std::vector<Trigger> triggers;
    std::vector<Policy> policies;

    QString qualifiedName() const { return schema + QLatin1Char('.') + name; }
    bool isTable() const { return kind == 'r' || kind == 'p' || kind == 'f'; }
    bool isView() const { return kind == 'v' || kind == 'm'; }
    bool isPartition() const { return !parentName.isEmpty(); }
    const Column *column(const QString &name) const;
    const Constraint *primaryKey() const;
};

struct Sequence
{
    QString schema;
    QString name;
    QString type;
    qint64 start = 1;
    qint64 increment = 1;
    qint64 minimum = 1;
    qint64 maximum = 0;
    qint64 cache = 1;
    bool cycle = false;
    QString ownedBy; // schema.table.column of a serial or identity column.
    QString owner;
    QString comment;

    QString qualifiedName() const { return schema + QLatin1Char('.') + name; }
};

struct Routine
{
    Oid oid = 0;
    QString schema;
    QString name;
    char kind = 'f'; // pg_proc.prokind: f, p, a, w.
    QString arguments; // pg_get_function_identity_arguments()
    QString result;
    QString language;
    char volatility = 'v'; // i, s, v
    bool securityDefiner = false;
    QString owner;
    QString comment;
    QString definition; // pg_get_functiondef(); empty for aggregates.
    QString body; // prosrc: the source alone, without the header.

    QString qualifiedName() const { return schema + QLatin1Char('.') + name; }
    QString signature() const { return name + QLatin1Char('(') + arguments + QLatin1Char(')'); }
};

struct UserType
{
    Oid oid = 0;
    QString schema;
    QString name;
    char kind = 'e'; // pg_type.typtype: e (enum), c (composite), d (domain), r (range).
    QString owner;
    QString comment;
    QStringList labels; // Of an enum, in their order.
    std::vector<Column> attributes; // Of a composite type.
    QString baseType; // Of a domain; a range's subtype.
    bool notNull = false; // Of a domain.
    QString defaultValue;
    std::vector<Constraint> constraints; // A domain's checks.

    QString qualifiedName() const { return schema + QLatin1Char('.') + name; }
};

struct Extension
{
    QString name;
    QString version;
    QString schema;
    QString comment;
};

struct Schema
{
    QString name;
    QString owner;
    QString comment;
};

// A view reading from a relation.
struct Dependency
{
    Oid view = 0;
    Oid relation = 0;
};

struct Model
{
    QString database;
    QString serverVersion;
    QString owner;
    QString encoding;
    QString comment;
    std::vector<Schema> schemas;
    std::vector<Relation> relations;
    std::vector<Sequence> sequences;
    std::vector<Routine> routines;
    std::vector<UserType> types;
    std::vector<Extension> extensions;
    std::vector<Dependency> dependencies;

    const Relation *relation(Oid oid) const;
    const Relation *relation(const QString &schema, const QString &name) const;
};

// Which schemas to document: names or patterns with * and ?, like
// pg_dump's -n and -N. Without any included, every schema but the
// server's own.
struct SchemaFilter
{
    QStringList include;
    QStringList exclude;
};

// Which schemas, as an SQL condition on n.nspname.
QByteArray schemaCondition(const SchemaFilter &filter);

// The queries reading a model, to run in order; their results go to
// parseModel() in the same order. Objects that belong to an extension are
// left out: they are the extension's, documented elsewhere.
std::vector<QByteArray> modelQueries(const SchemaFilter &filter);
Model parseModel(const std::vector<pg::Result> &results);

// Quotes an identifier the way PostgreSQL needs it, only when it needs it.
QString quoteIdentifier(const QString &name);
QString quoteQualified(const QString &schema, const QString &name);

// A list of schema names in the database, for choosing among them.
QByteArray schemaListQuery();
QStringList parseSchemaList(const pg::Result &result);

} // namespace slonisko::docgen
