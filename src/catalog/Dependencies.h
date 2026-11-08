// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::catalog {

// What depends on an object, transitively: what a DROP of it, or a change
// of a column's type, would hit. Most of it is in pg_depend; what function
// bodies refer to only by name is read from the bodies themselves.

struct QualifiedName
{
    QString schema; // Empty when the body does not say.
    QString name;

    bool operator==(const QualifiedName &) const = default;
};

// The objects a function's body names, as far as libpg_query can read it.
struct BodyReferences
{
    // When not, the tables and functions are guessed from the body's words.
    bool parsed = false;
    QString error; // Why it could not be parsed, when it could not.
    std::vector<QualifiedName> relations; // FROM, INSERT INTO, %ROWTYPE, nextval('…'), …
    std::vector<QualifiedName> functions;
    std::vector<QualifiedName> types;
    // Column names the body mentions anywhere, whatever table they are of;
    // star when it selects or returns whole rows.
    QStringList columns;
    bool star = false;
};

// language is "sql" or "plpgsql"; body is the function's source, prosrc.
// PL/pgSQL is parsed from the whole CREATE FUNCTION, as pg_get_functiondef()
// gives it, because the parser needs the arguments to tell variables from
// names. libpg_query does not know the variables' types, so what they name
// is missed, and a type qualified by a schema other than public makes it
// give up altogether.
BodyReferences bodyReferences(const QString &language, const QByteArray &body,
                              const QByteArray &definition = {});

// A search_path setting as the list of schemas it names, "$user" replaced.
QStringList splitSearchPath(const QString &setting, const QString &user);

struct DependencyObject
{
    Oid classId = 0; // The catalog it is in: pg_class, pg_proc, …
    Oid oid = 0;
    QString type; // As pg_identify_object() says: "view", "table constraint", …
    QString schema;
    QString name; // Its own name, unqualified; empty for objects named by what they are on.
    QString identity; // "public.orders", "orders_pkey on public.orders", "public.f(integer)"
    // relkind, prokind, typtype or contype, as applies.
    char kind = 0;
    bool system = false;
};

// The page to open for an object, if it has one.
std::optional<ObjectKind> objectKindOf(const DependencyObject &object);

// dependent needs referenced. Columns are those of a relation either side
// names, empty when the edge is about the whole object.
struct DependencyEdge
{
    int dependent = -1;
    int referenced = -1;
    QString dependentColumn;
    QString referencedColumn;
    char type = 0; // pg_depend.deptype; 0 for a name found in a function body.
    bool viaDefault = false; // The dependent is the default or expression of dependentColumn.
    // The edge was to the referenced relation's row type, which every column
    // of it is part of.
    bool viaRowType = false;
    // For edges from bodies: the column names the body mentions.
    QStringList bodyColumns;
    bool bodyStar = false;
};

struct DependencyGraph
{
    std::vector<DependencyObject> objects;
    std::vector<DependencyEdge> edges; // Between owners only, see owner.
    // What each object is part of, internally: a view for its _RETURN rule,
    // a table for its row type, a constraint for its index. Itself if nothing.
    std::vector<int> owner;
    std::vector<std::vector<int>> dependents; // Edges by referenced object.
    int root = -1;
    QStringList rootColumns; // When the root is a relation.
    // Functions whose bodies could not be read: what they refer to is not known.
    QStringList unreadable;

    // The object, by the catalog it is in and its oid; -1 if not there.
    int find(Oid classId, Oid oid) const;
    QHash<quint64, int> index; // find()'s lookup: classId in the high half.
};

// What happens to a dependent when the root is dropped.
enum class DropEffect {
    Cascade, // DROP refuses without CASCADE; with it, this goes too.
    DroppedAlong, // Goes with it without asking: an index, a trigger, a partition.
    FailsWhenRun, // Stays, and breaks the first time it runs: a function body names it.
};

struct Dependent
{
    int object = -1;
    int parent = -1; // Index in Impact::dependents; -1 under the root.
    int depth = 1;
    QString reason; // How it depends on its parent: "foreign key references it".
    QStringList columns; // The parent's columns it depends on, if it says.
    DropEffect effect = DropEffect::Cascade;
};

struct Impact
{
    // Every dependent once, by the shortest way, breadth first: parents
    // before their children.
    std::vector<Dependent> dependents;
    int cascade = 0; // How many make a plain DROP refuse.
    int dropped = 0; // How many DROP ... CASCADE would take along.
    int failing = 0; // How many stay and fail when run.
};

// Whether an object of this kind can be looked at this way.
bool hasDependencies(ObjectKind kind);

// The queries to run, in order, for parseDependencies(). They read the whole
// database's dependencies: what follows from one object is only known when
// everything that might depend on it is.
std::vector<QByteArray> dependencyQueries(ObjectKind kind, Oid oid);
// Parses function bodies too, so it may take a while on a big database.
DependencyGraph parseDependencies(ObjectKind kind, Oid oid, const std::vector<pg::Result> &results);

// What depends on the graph's root, or with column on just that column of
// it: what a change of the column's type would hit. Beyond the first step
// whole objects count, so that may list more than the change needs.
Impact impactOf(const DependencyGraph &graph, const QString &column = {});

QString dropEffectName(DropEffect effect);
// A few sentences on what the impact comes to, for above the list of it.
QString impactSummary(const DependencyGraph &graph, const Impact &impact, bool column);

} // namespace slonisko::catalog
