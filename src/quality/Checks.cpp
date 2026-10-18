// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Checks.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace slonisko::quality {

using docgen::Column;
using docgen::Constraint;
using docgen::Index;
using docgen::Model;
using docgen::quoteIdentifier;
using docgen::quoteQualified;
using docgen::Relation;
using docgen::Routine;
using docgen::Sequence;
using docgen::Trigger;

namespace {

QStringList keysOf(const Index &index)
{
    return index.columns.mid(0, index.keyColumns > 0 ? index.keyColumns : index.columns.size());
}

QStringList includedOf(const Index &index)
{
    return index.keyColumns > 0 ? index.columns.mid(index.keyColumns) : QStringList();
}

// The key columns as they sort: each with its operator class, collation,
// direction and nulls. Reversed, as a backward scan reads them: DESC and
// NULLS FIRST both flip, so ASC NULLS LAST becomes DESC NULLS FIRST.
QStringList sortedKeys(const Index &index, bool reversed = false)
{
    QStringList keys = keysOf(index);
    for (qsizetype i = 0; i < keys.size(); ++i) {
        int option = i < qsizetype(index.keyOptions.size()) ? index.keyOptions[std::size_t(i)] : 0;
        if (reversed)
            option ^= 3;
        keys[i] += QLatin1Char('\x1f') + index.keyQualifiers.value(i) + QLatin1Char('\x1f')
            + QString::number(option & 3);
    }
    return keys;
}

// Whether the keys of one index, in the order they sort, are the first
// keys of another, read forwards or, for a btree, backwards.
bool leadingKeys(const Index &index, const Index &other)
{
    const QStringList keys = sortedKeys(index);
    if (sortedKeys(other).mid(0, keys.size()) == keys)
        return true;
    return index.method == QLatin1String("btree")
        && sortedKeys(other, true).mid(0, keys.size()) == keys;
}

bool contains(const QStringList &columns, const QStringList &wanted)
{
    return std::ranges::all_of(wanted, [&](const QString &c) { return columns.contains(c); });
}

QString describe(const Index &index)
{
    QString text = keysOf(index).join(QLatin1String(", "));
    const QStringList included = includedOf(index);
    if (!included.isEmpty())
        text += QStringLiteral(" INCLUDE ") + included.join(QLatin1String(", "));
    return text;
}

bool usable(const Index &index)
{
    return index.valid && !index.partial;
}

// An index serves a foreign key when the key's columns, in any order, are
// the first ones it has: a lookup by the key can use it either way.
bool coversKey(const Index &index, const QStringList &columns)
{
    const QStringList keys = keysOf(index);
    if (!usable(index) || keys.size() < columns.size())
        return false;
    QSet<QString> wanted;
    for (const QString &column : columns)
        wanted.insert(quoteIdentifier(column));
    QSet<QString> leading;
    for (qsizetype i = 0; i < columns.size(); ++i)
        leading.insert(keys[i]);
    return wanted == leading;
}

QString columnList(const QStringList &columns)
{
    QStringList quoted;
    for (const QString &column : columns)
        quoted << quoteIdentifier(column);
    return quoted.join(QLatin1String(", "));
}

void checkPrimaryKey(const Relation &relation, std::vector<Finding> &findings)
{
    if ((relation.kind != 'r' && relation.kind != 'p') || relation.primaryKey())
        return;
    const bool unique = std::ranges::any_of(relation.constraints,
                                            [](const Constraint &c) { return c.type == 'u'; });
    findings.push_back({Check::NoPrimaryKey,
                        relation.schema,
                        relation.name,
                        {},
                        unique ? QStringLiteral("No primary key, though there is a unique "
                                                "constraint that could become one.")
                               : QStringLiteral("No primary key."),
                        {}});
}

void checkForeignKeys(const Model &model, const Relation &relation, std::vector<Finding> &findings)
{
    for (const Constraint &constraint : relation.constraints) {
        if (constraint.type != 'f')
            continue;
        const bool indexed = std::ranges::any_of(relation.indexes, [&](const Index &index) {
            return coversKey(index, constraint.columns);
        });
        if (!indexed)
            findings.push_back(
                {Check::UnindexedForeignKey, relation.schema, relation.name, constraint.name,
                 QStringLiteral("No index begins with (%1): deleting or updating a row of %2 "
                                "has to scan this table.")
                     .arg(constraint.columns.join(QLatin1String(", ")),
                          constraint.referencedSchema + QLatin1Char('.')
                              + constraint.referencedTable),
                 QStringLiteral("CREATE INDEX ON %1 (%2);")
                     .arg(quoteQualified(relation.schema, relation.name),
                          columnList(constraint.columns))});

        const Relation *target = model.relation(constraint.referencedOid);
        if (!target)
            continue;
        for (qsizetype i = 0;
             i < constraint.columns.size() && i < constraint.referencedColumns.size(); ++i) {
            const Column *from = relation.column(constraint.columns[i]);
            const Column *to = target->column(constraint.referencedColumns[i]);
            if (from && to && from->type != to->type)
                findings.push_back(
                    {Check::ForeignKeyTypeMismatch,
                     relation.schema,
                     relation.name,
                     constraint.name,
                     QStringLiteral("%1 is %2, but %3.%4 it references is %5.")
                         .arg(from->name, from->type, target->qualifiedName(), to->name, to->type),
                     {}});
        }
    }
}

void checkIndexes(const Relation &relation, std::vector<Finding> &findings)
{
    for (const Index &index : relation.indexes) {
        if (!index.valid)
            findings.push_back({Check::InvalidIndex, relation.schema, relation.name, index.name,
                                QStringLiteral("Invalid, probably left by a failed CREATE INDEX "
                                               "CONCURRENTLY: it is kept up to date but never "
                                               "used."),
                                QStringLiteral("REINDEX INDEX CONCURRENTLY %1;")
                                    .arg(quoteQualified(relation.schema, index.name))});
    }
    if (relation.isPartition())
        return;

    for (const Index &index : relation.indexes) {
        if (!usable(index) || index.primary)
            continue;
        const qsizetype keys = keysOf(index).size();
        const QStringList included = includedOf(index);
        for (const Index &other : relation.indexes) {
            if (&other == &index || !usable(other) || other.method != index.method
                || !leadingKeys(index, other))
                continue;
            Check check;
            if (keysOf(other).size() == keys) {
                // The other one has to have at least the columns this one
                // INCLUDEs, or this one still serves index-only scans it
                // cannot.
                const QStringList otherIncluded = includedOf(other);
                if (!contains(otherIncluded, included))
                    continue;
                // Of two alike, the one enforcing nothing goes, or else the
                // later one by name; of one with more columns INCLUDEd, the
                // one with fewer, unless only it enforces something.
                const bool same = otherIncluded.size() == included.size();
                const bool goes = same ? (!index.unique && other.unique)
                        || (index.unique == other.unique && index.name > other.name)
                                       : !index.unique || other.unique;
                if (!goes)
                    continue;
                check = Check::DuplicateIndex;
            } else if (!index.unique && index.method == QLatin1String("btree")
                       && contains(other.columns, included)) {
                check = Check::RedundantIndex;
            } else {
                continue;
            }
            findings.push_back({check, relation.schema, relation.name, index.name,
                                QStringLiteral("%1 covers the same columns (%2).")
                                    .arg(other.name, describe(other)),
                                QStringLiteral("DROP INDEX %1;")
                                    .arg(quoteQualified(relation.schema, index.name))});
            break;
        }
    }
}

// From the PostgreSQL wiki's "Don't Do This".
QString typeAdvice(const Column &column)
{
    const QString &type = column.type;
    if (type == QLatin1String("character") || type.startsWith(QLatin1String("character(")))
        return QStringLiteral("char(n) pads with spaces and is no faster; text or varchar is "
                              "the usual choice.");
    if (type == QLatin1String("money"))
        return QStringLiteral("money depends on lc_monetary and rounds; numeric holds amounts "
                              "exactly.");
    if (type.startsWith(QLatin1String("timestamp"))
        && type.endsWith(QLatin1String("without time zone")))
        return QStringLiteral("timestamp without time zone records no point in time; "
                              "timestamptz does.");
    if (type.startsWith(QLatin1String("time")) && type.endsWith(QLatin1String("with time zone"))
        && !type.startsWith(QLatin1String("timestamp")))
        return QStringLiteral("timetz has a time zone but no date, so no daylight saving time; "
                              "the documentation itself advises against it.");
    if (column.defaultValue.startsWith(QLatin1String("nextval(")) && column.identity.isEmpty())
        return QStringLiteral("A serial column; an identity column (GENERATED ... AS IDENTITY) "
                              "is the standard way, and its sequence follows the table's "
                              "permissions.");
    return {};
}

void checkTypes(const Relation &relation, std::vector<Finding> &findings)
{
    if (!relation.isTable() || relation.isPartition())
        return;
    for (const Column &column : relation.columns) {
        const QString advice = typeAdvice(column);
        if (!advice.isEmpty())
            findings.push_back({Check::DiscouragedType,
                                relation.schema,
                                relation.name,
                                column.name,
                                column.type + QLatin1String(": ") + advice,
                                {}});
    }
}

void checkConstraints(const Relation &relation, std::vector<Finding> &findings)
{
    const QString table = quoteQualified(relation.schema, relation.name);
    for (const Constraint &constraint : relation.constraints) {
        if (constraint.definition.endsWith(QLatin1String(" NOT VALID")))
            findings.push_back({Check::NotValidConstraint, relation.schema, relation.name,
                                constraint.name,
                                QStringLiteral("Added NOT VALID and never validated: the rows "
                                               "that were there before are not checked."),
                                QStringLiteral("ALTER TABLE %1 VALIDATE CONSTRAINT %2;")
                                    .arg(table, quoteIdentifier(constraint.name))});
    }
    for (std::size_t i = 0; i < relation.constraints.size(); ++i) {
        const Constraint &constraint = relation.constraints[i];
        for (std::size_t j = 0; j < i; ++j) {
            const Constraint &earlier = relation.constraints[j];
            if (earlier.type != constraint.type || earlier.definition != constraint.definition)
                continue;
            findings.push_back(
                {Check::DuplicateConstraint, relation.schema, relation.name, constraint.name,
                 QStringLiteral("The same as %1: %2").arg(earlier.name, constraint.definition),
                 QStringLiteral("ALTER TABLE %1 DROP CONSTRAINT %2;")
                     .arg(table, quoteIdentifier(constraint.name))});
            break;
        }
    }
}

// A trigger's definition without its own name, to compare it with others.
QString triggerBody(const Trigger &trigger)
{
    QString definition = trigger.definition;
    const QString name = quoteIdentifier(trigger.name);
    const qsizetype at = definition.indexOf(QLatin1Char(' ') + name + QLatin1Char(' '));
    if (at >= 0)
        definition.remove(at + 1, name.size());
    return definition;
}

void checkTriggers(const Relation &relation, std::vector<Finding> &findings)
{
    const QString table = quoteQualified(relation.schema, relation.name);
    for (std::size_t i = 0; i < relation.triggers.size(); ++i) {
        const Trigger &trigger = relation.triggers[i];
        if (!trigger.enabled)
            findings.push_back(
                {Check::DisabledTrigger, relation.schema, relation.name, trigger.name,
                 QStringLiteral("Disabled: %1() is not called.").arg(trigger.function),
                 QStringLiteral("ALTER TABLE %1 ENABLE TRIGGER %2;")
                     .arg(table, quoteIdentifier(trigger.name))});
        for (std::size_t j = 0; j < i; ++j) {
            if (triggerBody(relation.triggers[j]) != triggerBody(trigger))
                continue;
            findings.push_back({Check::DuplicateTrigger, relation.schema, relation.name,
                                trigger.name,
                                QStringLiteral("Fires on the same events as %1 and calls the same "
                                               "%2(), so it runs twice.")
                                    .arg(relation.triggers[j].name, trigger.function),
                                QStringLiteral("DROP TRIGGER %1 ON %2;")
                                    .arg(quoteIdentifier(trigger.name), table)});
            break;
        }
    }
}

void checkRowSecurity(const Relation &relation, std::vector<Finding> &findings)
{
    const QString table = quoteQualified(relation.schema, relation.name);
    if (relation.rowSecurity && relation.policies.empty())
        findings.push_back({Check::RowSecurity,
                            relation.schema,
                            relation.name,
                            {},
                            QStringLiteral("Row level security is on, but there is no policy: "
                                           "nobody but the owner sees a row."),
                            {}});
    else if (!relation.rowSecurity && !relation.policies.empty())
        findings.push_back(
            {Check::RowSecurity,
             relation.schema,
             relation.name,
             {},
             QStringLiteral("There are policies, but row level security is off, "
                            "so they are ignored."),
             QStringLiteral("ALTER TABLE %1 ENABLE ROW LEVEL SECURITY;").arg(table)});
}

void checkNames(const Relation &relation, std::vector<Finding> &findings)
{
    if (quoteIdentifier(relation.name) != relation.name)
        findings.push_back(
            {Check::NeedsQuoting,
             relation.schema,
             relation.name,
             {},
             QStringLiteral("Every query has to spell it %1.").arg(quoteIdentifier(relation.name)),
             {}});
    QHash<QString, QString> seen;
    for (const Column &column : relation.columns) {
        if (quoteIdentifier(column.name) != column.name)
            findings.push_back({Check::NeedsQuoting,
                                relation.schema,
                                relation.name,
                                column.name,
                                QStringLiteral("Every query has to spell it %1.")
                                    .arg(quoteIdentifier(column.name)),
                                {}});
        const QString folded = column.name.toLower();
        if (const auto it = seen.constFind(folded); it != seen.cend())
            findings.push_back({Check::NameCaseClash,
                                relation.schema,
                                relation.name,
                                column.name,
                                QStringLiteral("Differs from the column %1 only in case.").arg(*it),
                                {}});
        else
            seen.insert(folded, column.name);
    }
}

bool isIntegerType(const QString &type)
{
    return type == QLatin1String("smallint") || type == QLatin1String("integer")
        || type == QLatin1String("bigint");
}

void checkSequences(const Model &model, std::vector<Finding> &findings)
{
    for (const Sequence &sequence : model.sequences) {
        const QString qualified = quoteQualified(sequence.schema, sequence.name);
        if (sequence.ownedBy.isEmpty()) {
            // What would use it names it, as nextval('name') or
            // nextval('schema.name').
            const auto uses = [&](const QString &text) { return text.contains(sequence.name); };
            const bool used
                = std::ranges::any_of(model.relations,
                                      [&](const Relation &relation) {
                                          return uses(relation.definition)
                                              || std::ranges::any_of(
                                                     relation.columns, [&](const Column &column) {
                                                         return uses(column.defaultValue);
                                                     });
                                      })
                || std::ranges::any_of(model.routines, [&](const Routine &routine) {
                       return uses(routine.definition);
                   });
            if (!used)
                findings.push_back({Check::UnusedSequence,
                                    sequence.schema,
                                    {},
                                    sequence.name,
                                    QStringLiteral("No column, view or routine uses it, and no "
                                                   "column owns it."),
                                    {}});
            continue;
        }
        const QStringList owner = sequence.ownedBy.split(QLatin1Char('.'));
        if (owner.size() != 3)
            continue;
        const Relation *table = model.relation(owner[0], owner[1]);
        const Column *column = table ? table->column(owner[2]) : nullptr;
        if (!column || !isIntegerType(column->type) || !isIntegerType(sequence.type)
            || column->type == sequence.type)
            continue;
        const auto width = [](const QString &type) {
            return type == QLatin1String("smallint") ? 2 : type == QLatin1String("integer") ? 4 : 8;
        };
        const bool columnNarrower = width(column->type) < width(sequence.type);
        findings.push_back(
            {Check::SequenceTypeMismatch, table->schema, table->name, column->name,
             columnNarrower
                 ? QStringLiteral("%1 is %2, but its sequence %3 is %4: inserts fail once the "
                                  "sequence passes what %2 holds.")
                       .arg(column->name, column->type, sequence.name, sequence.type)
                 : QStringLiteral("%1 is %2, but its sequence %3 is %4 and runs out first.")
                       .arg(column->name, column->type, sequence.name, sequence.type),
             columnNarrower
                 ? QStringLiteral("ALTER TABLE %1 ALTER COLUMN %2 TYPE %3;")
                       .arg(quoteQualified(table->schema, table->name),
                            quoteIdentifier(column->name), sequence.type)
                 : QStringLiteral("ALTER SEQUENCE %1 AS %2;").arg(qualified, column->type)});
    }
}

// Definitions alike but for whitespace, short ones aside: a stub like
// "BEGIN RETURN NEW; END" may well be written twice on purpose.
QString comparable(const QString &definition)
{
    const QString simplified = definition.simplified();
    return simplified.size() < 40 ? QString() : simplified;
}

void checkIdentical(const Model &model, std::vector<Finding> &findings)
{
    QHash<QString, const Relation *> views;
    for (const Relation &relation : model.relations) {
        if (!relation.isView())
            continue;
        const QString key = comparable(relation.definition);
        if (key.isEmpty())
            continue;
        if (const auto it = views.constFind(key); it != views.cend())
            findings.push_back({Check::IdenticalDefinition,
                                relation.schema,
                                relation.name,
                                {},
                                QStringLiteral("The same query as %1.").arg((*it)->qualifiedName()),
                                {}});
        else
            views.insert(key, &relation);
    }
    QHash<QString, const Routine *> routines;
    for (const Routine &routine : model.routines) {
        if (routine.kind == 'a' || routine.language == QLatin1String("c")
            || routine.language == QLatin1String("internal"))
            continue;
        QString key = comparable(routine.body);
        if (key.isEmpty())
            continue;
        key = routine.language + QLatin1Char('\n') + key;
        if (const auto it = routines.constFind(key); it != routines.cend())
            findings.push_back(
                {Check::IdenticalDefinition,
                 routine.schema,
                 {},
                 routine.signature(),
                 QStringLiteral("The same body as %1.%2.").arg((*it)->schema, (*it)->signature()),
                 {}});
        else
            routines.insert(key, &routine);
    }
}

void checkTableNames(const Model &model, std::vector<Finding> &findings)
{
    QHash<QString, const Relation *> seen;
    for (const Relation &relation : model.relations) {
        const QString key = relation.schema + QLatin1Char('.') + relation.name.toLower();
        if (const auto it = seen.constFind(key); it != seen.cend())
            findings.push_back({Check::NameCaseClash,
                                relation.schema,
                                relation.name,
                                {},
                                QStringLiteral("Differs from %1 only in case.").arg((*it)->name),
                                {}});
        else
            seen.insert(key, &relation);
    }
}

void checkRoutines(const Model &model, std::vector<Finding> &findings)
{
    for (const Routine &routine : model.routines) {
        // pg_get_functiondef() writes a SET clause of its own for it.
        if (!routine.securityDefiner
            || routine.definition.contains(QLatin1String("SET search_path"), Qt::CaseInsensitive))
            continue;
        findings.push_back(
            {Check::SecurityDefiner,
             routine.schema,
             {},
             routine.signature(),
             QStringLiteral("SECURITY DEFINER without its own search_path: whoever calls it "
                            "chooses which tables and functions its names find."),
             QStringLiteral("ALTER %1 %2(%3) SET search_path = %4, pg_temp;")
                 .arg(routine.kind == 'p' ? QStringLiteral("PROCEDURE")
                                          : QStringLiteral("FUNCTION"),
                      quoteQualified(routine.schema, routine.name), routine.arguments,
                      quoteIdentifier(routine.schema))});
    }
}

} // namespace

std::vector<Check> allChecks()
{
    return {Check::NoPrimaryKey,         Check::UnindexedForeignKey, Check::ForeignKeyTypeMismatch,
            Check::InvalidIndex,         Check::DuplicateIndex,      Check::RedundantIndex,
            Check::DuplicateConstraint,  Check::NotValidConstraint,  Check::DisabledTrigger,
            Check::DuplicateTrigger,     Check::RowSecurity,         Check::SecurityDefiner,
            Check::SequenceTypeMismatch, Check::UnusedSequence,      Check::IdenticalDefinition,
            Check::NameCaseClash,        Check::NeedsQuoting,        Check::DiscouragedType,
            Check::Undocumented};
}

QString checkTitle(Check check)
{
    switch (check) {
    case Check::NoPrimaryKey:
        return QStringLiteral("Tables without a primary key");
    case Check::UnindexedForeignKey:
        return QStringLiteral("Foreign keys without an index");
    case Check::ForeignKeyTypeMismatch:
        return QStringLiteral("Foreign keys between different types");
    case Check::InvalidIndex:
        return QStringLiteral("Invalid indexes");
    case Check::DuplicateIndex:
        return QStringLiteral("Duplicate indexes");
    case Check::RedundantIndex:
        return QStringLiteral("Redundant indexes");
    case Check::DuplicateConstraint:
        return QStringLiteral("Duplicate constraints");
    case Check::NotValidConstraint:
        return QStringLiteral("Constraints not validated");
    case Check::DisabledTrigger:
        return QStringLiteral("Disabled triggers");
    case Check::DuplicateTrigger:
        return QStringLiteral("Duplicate triggers");
    case Check::RowSecurity:
        return QStringLiteral("Row level security half set up");
    case Check::SecurityDefiner:
        return QStringLiteral("SECURITY DEFINER without a search_path");
    case Check::SequenceTypeMismatch:
        return QStringLiteral("Sequences of another type than their column");
    case Check::UnusedSequence:
        return QStringLiteral("Sequences nothing uses");
    case Check::IdenticalDefinition:
        return QStringLiteral("Views and routines defined twice");
    case Check::NameCaseClash:
        return QStringLiteral("Names differing only in case");
    case Check::NeedsQuoting:
        return QStringLiteral("Names that need quoting");
    case Check::DiscouragedType:
        return QStringLiteral("Column types to reconsider");
    case Check::Undocumented:
        return QStringLiteral("Objects without a comment");
    }
    return {};
}

QString checkExplanation(Check check)
{
    switch (check) {
    case Check::NoPrimaryKey:
        return QStringLiteral("Without one, a row cannot be told apart from its duplicates, "
                              "logical replication cannot replicate updates and deletes, and "
                              "most tools cannot edit the table.");
    case Check::UnindexedForeignKey:
        return QStringLiteral("PostgreSQL indexes the referenced side of a foreign key, not "
                              "the referencing one. Deleting a referenced row, or changing its "
                              "key, then scans the whole referencing table, and so do joins "
                              "along the key.");
    case Check::ForeignKeyTypeMismatch:
        return QStringLiteral("Every comparison along the key needs a cast, and the referenced "
                              "side can hold values the referencing side cannot.");
    case Check::InvalidIndex:
        return QStringLiteral("An index a failed concurrent build left behind: written to on "
                              "every change, never read.");
    case Check::DuplicateIndex:
        return QStringLiteral("Another index has the very same columns, so this one only costs "
                              "writes and space. Of two alike, a unique one is kept, since it "
                              "enforces something.");
    case Check::RedundantIndex:
        return QStringLiteral("Another index has the same columns first and more after them, "
                              "so it can do this one's work. Unique ones are kept, since they "
                              "enforce something.");
    case Check::DuplicateConstraint:
        return QStringLiteral("The same constraint twice on a table: checked twice on every "
                              "change, a foreign key with a trigger of its own each time.");
    case Check::NotValidConstraint:
        return QStringLiteral("A constraint added NOT VALID holds for new rows only, until it "
                              "is validated; the planner cannot rely on it meanwhile.");
    case Check::DisabledTrigger:
        return QStringLiteral("A disabled trigger is easy to forget, and whatever it was there "
                              "to keep up is not kept up meanwhile.");
    case Check::DuplicateTrigger:
        return QStringLiteral("Two triggers on the same events calling the same function do the "
                              "work twice, usually since one was created again under a new "
                              "name.");
    case Check::RowSecurity:
        return QStringLiteral("Row level security needs both the switch on the table and at "
                              "least one policy; one without the other hides every row or "
                              "none.");
    case Check::SecurityDefiner:
        return QStringLiteral("A SECURITY DEFINER routine runs with its owner's rights but "
                              "looks names up in its caller's search_path, so a caller who can "
                              "create objects can make it run theirs. Fixing the search_path "
                              "closes that.");
    case Check::SequenceTypeMismatch:
        return QStringLiteral("A serial or identity column's sequence should be its column's "
                              "type; otherwise one of the two runs out of numbers before the "
                              "other.");
    case Check::UnusedSequence:
        return QStringLiteral("A sequence no column default, view or routine in these schemas "
                              "mentions. An application may still call it by name.");
    case Check::IdenticalDefinition:
        return QStringLiteral("Two views with the same query, or two routines with the same "
                              "body: copies that will drift apart once one of them changes.");
    case Check::NameCaseClash:
        return QStringLiteral("Names that only quoting tells apart, a trap for anyone writing "
                              "a query, and for case-insensitive tools.");
    case Check::NeedsQuoting:
        return QStringLiteral("Upper case letters, spaces or reserved words make a name that "
                              "has to be quoted in every query, exactly as it is spelt.");
    case Check::DiscouragedType:
        return QStringLiteral("Types the PostgreSQL wiki's \"Don't Do This\" page advises "
                              "against, with the reason it gives.");
    case Check::Undocumented:
        return QStringLiteral("Tables, views and routines nobody has written a COMMENT ON for.");
    }
    return {};
}

std::vector<Finding> runChecks(const Model &model)
{
    std::vector<Finding> findings;
    for (const Relation &relation : model.relations) {
        if (relation.isTable() && !relation.isPartition()) {
            checkPrimaryKey(relation, findings);
            checkForeignKeys(model, relation, findings);
            checkConstraints(relation, findings);
            checkRowSecurity(relation, findings);
        }
        checkIndexes(relation, findings);
        checkTriggers(relation, findings);
        checkTypes(relation, findings);
        if (!relation.isPartition())
            checkNames(relation, findings);
    }
    checkTableNames(model, findings);
    checkSequences(model, findings);
    checkIdentical(model, findings);
    checkRoutines(model, findings);
    for (const Relation &relation : model.relations) {
        if (relation.comment.isEmpty() && !relation.isPartition())
            findings.push_back(
                {Check::Undocumented,
                 relation.schema,
                 relation.name,
                 {},
                 QStringLiteral("No comment."),
                 QStringLiteral("COMMENT ON %1 %2 IS '...';")
                     .arg(relation.kind == 'v'       ? QStringLiteral("VIEW")
                              : relation.kind == 'm' ? QStringLiteral("MATERIALIZED VIEW")
                              : relation.kind == 'f' ? QStringLiteral("FOREIGN TABLE")
                                                     : QStringLiteral("TABLE"),
                          quoteQualified(relation.schema, relation.name))});
    }
    for (const Routine &routine : model.routines) {
        if (routine.comment.isEmpty() && routine.kind != 'a')
            findings.push_back(
                {Check::Undocumented,
                 routine.schema,
                 {},
                 routine.signature(),
                 QStringLiteral("No comment."),
                 QStringLiteral("COMMENT ON %1 %2(%3) IS '...';")
                     .arg(routine.kind == 'p' ? QStringLiteral("PROCEDURE")
                                              : QStringLiteral("FUNCTION"),
                          quoteQualified(routine.schema, routine.name), routine.arguments)});
    }
    std::ranges::stable_sort(findings, {}, [](const Finding &f) { return int(f.check); });
    return findings;
}

} // namespace slonisko::quality
