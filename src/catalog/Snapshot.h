// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace slonisko::catalog {

using Oid = unsigned int;

struct Column
{
    QString name;
    QString type; // As format_type() spells it, like "character varying(20)".
};

struct Relation
{
    Oid oid = 0;
    QString schema;
    QString name;
    char kind = 'r'; // pg_class.relkind
    std::vector<Column> columns;
};

struct Function
{
    QString schema;
    QString name;
    QString arguments; // pg_get_function_identity_arguments()
    QString result;
    char kind = 'f'; // pg_proc.prokind
};

struct Type
{
    QString schema;
    QString name; // As format_type() spells it, like "integer".
};

// Immutable view of one database's catalog, for completion. The loader
// builds a new snapshot and swaps the pointer; readers, including
// completion threads, never lock.
struct Snapshot
{
    int serverVersion = 0;
    QStringList searchPath; // Effective, with pg_catalog: current_schemas(true).
    QStringList schemas;
    std::vector<Relation> relations;
    std::vector<Function> functions;
    std::vector<Type> types;

    // Resolves an unqualified name via search_path, or a qualified one directly.
    const Relation *findRelation(const QString &schema, const QString &name) const;
    bool hasSchema(const QString &name) const { return schemas.contains(name); }
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// SQL reading everything a snapshot holds, in one round trip.
QByteArray snapshotQuery();

} // namespace slonisko::catalog

namespace slonisko::pg {
class Result;
}

namespace slonisko::catalog {

// Builds a snapshot from the results of snapshotQuery(), in order. Returns
// null if they do not look like its results.
SnapshotPtr parseSnapshot(const std::vector<pg::Result> &results, int serverVersion);

// Splits a PostgreSQL text array like {a,"b c"} into its elements.
QStringList parseTextArray(const QString &array);

} // namespace slonisko::catalog
