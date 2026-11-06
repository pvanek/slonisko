// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Model.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QObject;

namespace slonisko::pg {
class QueryRunner;
}

namespace slonisko::schemadiff {

// Compares the schemas of two databases and writes the SQL that makes the
// first like the second, after the manner of Stripe's pg-schema-diff: the
// script runs on the target, and the reference is the schema to match,
// which is only read.
//
// Both sides are docgen::Models read with their names qualified (see
// loadModel()), so objects are matched by schema and name, and their
// definitions compare as the server spells them.

enum class ChangeKind { Added, Removed, Changed };

enum class ObjectType {
    Schema,
    Extension,
    Type,
    Domain,
    Sequence,
    Function,
    Procedure,
    Table,
    View,
    MaterializedView,
    Column,
    Constraint,
    Index,
    Trigger,
    Policy,
};

// What running a change may do besides changing the schema; the script
// says so above the statements, and the wizard shows it.
enum class HazardType {
    DataLoss, // Rows or columns go away.
    Rewrite, // The table is rewritten, holding an exclusive lock meanwhile.
    Blocking, // Writes wait while the table is scanned or an index built.
    Manual, // Cannot be done in place; the statements are left commented out.
};

struct Hazard
{
    HazardType type;
    QString message;
};

struct Change
{
    ChangeKind kind = ChangeKind::Changed;
    ObjectType type = ObjectType::Table;
    QString
        name; // Qualified: "public.orders", "public.orders.total", "orders_pkey on public.orders"
    QString details; // What differs, for a change.
    std::vector<Hazard> hazards;
    QStringList statements; // This change's part of the script, in its order.
};

struct Options
{
    // CREATE INDEX CONCURRENTLY on tables that exist already, so writes go
    // on meanwhile; such a script cannot run in a transaction.
    bool concurrentIndexes = false;
};

struct Statement
{
    QString sql; // With its semicolon; commented out for a Manual hazard.
    int change = 0; // Index into Diff::changes.
    bool transactional = true; // False for what may not run in a transaction block.
    // Runs before the transaction: a new enum value cannot be used in the
    // transaction that added it.
    bool early = false;
};

struct Diff
{
    std::vector<Change> changes;
    // The whole script in the order it runs: drops first, dependents before
    // what they depend on, then creations the other way round.
    std::vector<Statement> statements;

    bool isEmpty() const { return changes.empty(); }
    bool transactional() const;
};

Diff compare(const docgen::Model &target, const docgen::Model &reference,
             const Options &options = {});

struct ScriptOptions
{
    QString targetName; // For the header: which database the script is for,
    QString referenceName; // and which it makes it like.
    // BEGIN and COMMIT around it, unless a statement cannot run in a
    // transaction block.
    bool transaction = true;
};

// The script, its header and the hazards included. lines, if given, gets
// for each change the first and last line (from 0) of each run of its
// statements.
QString script(const Diff &diff, const ScriptOptions &options,
               std::vector<std::vector<std::pair<int, int>>> *lines = nullptr);

QString kindName(ChangeKind kind);
QString typeName(ObjectType type);
QString hazardName(HazardType type);

// The databases of a server one can connect to, for choosing among them;
// docgen::parseSchemaList() reads the names.
QByteArray databaseListQuery();

// Reads a model to compare: docgen's, with qualified names. done is
// docgen::loadModel()'s.
void loadModel(pg::QueryRunner *runner, const docgen::SchemaFilter &filter, QObject *context,
               std::function<void(const docgen::Model &model, const QString &error)> done);

} // namespace slonisko::schemadiff
