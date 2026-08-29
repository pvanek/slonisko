// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"

#include <QByteArray>
#include <QString>

#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::catalog {

enum class ObjectKind {
    Database,
    Schema,
    Table,
    PartitionedTable,
    View,
    MaterializedView,
    ForeignTable,
    Sequence,
    Function,
    Procedure,
    Aggregate,
    Type,
    Domain,
    Column,
    Constraint,
    Index,
    Trigger,
    Policy,
    Extension,
    Publication,
    EventTrigger,
    Role,
    Tablespace,
};

// A group of objects in the browser, listed by one catalog query.
enum class Folder {
    // Server-wide.
    Databases,
    Roles,
    Tablespaces,
    // In a database.
    Schemas,
    Extensions,
    Publications,
    EventTriggers,
    // In a schema.
    Tables,
    Views,
    MaterializedViews,
    ForeignTables,
    Sequences,
    Functions,
    Procedures,
    Aggregates,
    Types,
    Domains,
    // In a table, view or similar.
    Columns,
    Constraints,
    Indexes,
    Triggers,
    Policies,
    Partitions,
};

struct DbObject
{
    ObjectKind kind = ObjectKind::Table;
    // The object's oid; for columns, the attribute number.
    Oid oid = 0;
    QString name;
    QString detail; // E.g. a column's type or a function's result type.
    bool system = false; // Built into PostgreSQL, like pg_catalog.
};

// The folders shown under an object of this kind, in display order.
std::vector<Folder> foldersOf(ObjectKind kind);
QString folderTitle(Folder folder);
QString kindName(ObjectKind kind);

// SQL listing the folder's objects. owner is the oid of the schema or
// relation the folder belongs to; it is unused for database- and
// server-wide folders.
QByteArray folderQuery(Folder folder, Oid owner = 0);
std::vector<DbObject> parseFolder(Folder folder, const pg::Result &result);

} // namespace slonisko::catalog
