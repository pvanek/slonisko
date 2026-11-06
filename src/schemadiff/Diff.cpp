// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "schemadiff/Diff.h"

#include "docgen/Generator.h"

#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <functional>

namespace slonisko::schemadiff {

using namespace docgen;

namespace {

// When each statement runs. What depends on others goes first when it is
// dropped and last when it is made; what others depend on is dropped only
// at the end, once nothing uses it any more, so that a column can move from
// an old type to a new one before the old one goes.
enum Phase {
    AddEnumValues,
    DropViews,
    DropTriggers,
    DropPolicies,
    DropForeignKeys,
    DropIndexes,
    DropConstraints,
    DetachPartitions,
    DropTables,
    DropColumns,
    CreateSchemas,
    CreateExtensions,
    CreateTypes,
    AlterTypes,
    CreateSequences,
    CreateRoutines,
    CreateTables,
    CreatePartitions,
    AlterTables,
    AttachPartitions,
    AlterSequences,
    AddConstraints,
    CreateIndexes,
    AddForeignKeys,
    CreateViews,
    CreateViewIndexes,
    CreateTriggers,
    CreatePolicies,
    Comments,
    DropRoutines,
    DropSequences,
    DropTypes,
    DropExtensions,
    DropSchemas,
};

QString literal(const QString &text)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString q(const QString &name)
{
    return quoteIdentifier(name);
}

QString qq(const QString &schema, const QString &name)
{
    return quoteQualified(schema, name);
}

QString qq(const Relation &relation)
{
    return qq(relation.schema, relation.name);
}

QString withSemicolon(const QString &sql)
{
    QString out = sql.trimmed();
    if (!out.endsWith(QLatin1Char(';')))
        out += QLatin1Char(';');
    return out;
}

QString withoutSemicolon(const QString &sql)
{
    QString out = sql.trimmed();
    while (out.endsWith(QLatin1Char(';')))
        out = out.chopped(1).trimmed();
    return out;
}

QString commentOn(const QString &what, const QString &text)
{
    return QStringLiteral("COMMENT ON %1 IS %2;")
        .arg(what, text.isEmpty() ? QStringLiteral("NULL") : literal(text));
}

QString relationKeyword(char kind)
{
    switch (kind) {
    case 'v':
        return QStringLiteral("VIEW");
    case 'm':
        return QStringLiteral("MATERIALIZED VIEW");
    default:
        return QStringLiteral("TABLE");
    }
}

ObjectType relationType(char kind)
{
    switch (kind) {
    case 'v':
        return ObjectType::View;
    case 'm':
        return ObjectType::MaterializedView;
    default:
        return ObjectType::Table;
    }
}

QString routineKeyword(const Routine &routine)
{
    switch (routine.kind) {
    case 'p':
        return QStringLiteral("PROCEDURE");
    case 'a':
        return QStringLiteral("AGGREGATE");
    default:
        return QStringLiteral("FUNCTION");
    }
}

QString routineName(const Routine &routine)
{
    return qq(routine.schema, routine.name) + QLatin1Char('(') + routine.arguments
        + QLatin1Char(')');
}

QString routineKey(const Routine &routine)
{
    return routine.qualifiedName() + QLatin1Char('(') + routine.arguments + QLatin1Char(')');
}

QString columnDefinition(const Column &column)
{
    QString sql = q(column.name) + QLatin1Char(' ') + column.type;
    if (!column.generated.isEmpty())
        sql += QStringLiteral(" GENERATED ALWAYS AS (%1) %2")
                   .arg(column.generated,
                        column.virtualGenerated ? QStringLiteral("VIRTUAL")
                                                : QStringLiteral("STORED"));
    if (!column.identity.isEmpty())
        sql += QStringLiteral(" GENERATED %1 AS IDENTITY").arg(column.identity);
    if (!column.defaultValue.isEmpty())
        sql += QStringLiteral(" DEFAULT ") + column.defaultValue;
    if (column.notNull)
        sql += QStringLiteral(" NOT NULL");
    return sql;
}

// reloptions as the model has them, "fillfactor=70, autovacuum_enabled=off".
QMap<QString, QString> parseOptions(const QString &options)
{
    QMap<QString, QString> out;
    for (const QString &option : options.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        const qsizetype equals = option.indexOf(QLatin1Char('='));
        if (equals > 0)
            out.insert(option.left(equals), option.mid(equals + 1));
    }
    return out;
}

QString policyRoles(const QStringList &roles)
{
    QStringList quoted;
    for (const QString &role : roles)
        quoted << (role == QLatin1String("public") ? QStringLiteral("PUBLIC") : q(role));
    return quoted.join(QStringLiteral(", "));
}

QString createPolicy(const Relation &relation, const Policy &policy)
{
    QString sql
        = QStringLiteral("CREATE POLICY %1 ON %2 AS %3 FOR %4")
              .arg(q(policy.name), qq(relation),
                   policy.permissive ? QStringLiteral("PERMISSIVE") : QStringLiteral("RESTRICTIVE"),
                   policy.command);
    if (!policy.roles.isEmpty())
        sql += QStringLiteral(" TO ") + policyRoles(policy.roles);
    if (!policy.using_.isEmpty())
        sql += QStringLiteral(" USING (%1)").arg(policy.using_);
    if (!policy.withCheck.isEmpty())
        sql += QStringLiteral(" WITH CHECK (%1)").arg(policy.withCheck);
    return sql + QLatin1Char(';');
}

bool samePolicy(const Policy &a, const Policy &b)
{
    return a.command == b.command && a.permissive == b.permissive && a.roles == b.roles
        && a.using_ == b.using_ && a.withCheck == b.withCheck;
}

// Indexes that a primary key, unique or exclusion constraint made; they go
// with the constraint.
QSet<QString> constraintIndexes(const Relation &relation)
{
    QSet<QString> names;
    for (const Constraint &constraint : relation.constraints) {
        if (constraint.type == 'p' || constraint.type == 'u' || constraint.type == 'x')
            names.insert(constraint.name);
    }
    return names;
}

template<typename T>
const T *byName(const std::vector<T> &objects, const QString &name)
{
    const auto it = std::ranges::find(objects, name, &T::name);
    return it == objects.end() ? nullptr : &*it;
}

class Builder
{
public:
    Builder(const Model &target, const Model &reference, const Options &options)
        : m_target(target), m_reference(reference), m_options(options)
    { }

    Diff run()
    {
        schemas();
        extensions();
        types();
        sequences();
        routines();
        relations();

        std::ranges::stable_sort(m_pending, {},
                                 [](const Pending &p) { return std::pair(p.phase, p.order); });
        Diff diff;
        diff.changes = std::move(m_changes);
        for (Pending &pending : m_pending) {
            diff.changes[pending.statement.change].statements << pending.statement.sql;
            diff.statements.push_back(std::move(pending.statement));
        }
        return diff;
    }

private:
    struct Pending
    {
        Phase phase;
        int order; // Within the phase: views and partitions by depth.
        Statement statement;
    };

    int add(ChangeKind kind, ObjectType type, const QString &name, const QString &details = {})
    {
        m_changes.push_back({kind, type, name, details, {}, {}});
        return int(m_changes.size()) - 1;
    }

    void emit(int change, Phase phase, const QString &sql, bool transactional = true, int order = 0)
    {
        m_pending.push_back({phase, order, {sql, change, transactional}});
    }

    void hazard(int change, HazardType type, const QString &message)
    {
        m_changes[change].hazards.push_back({type, message});
    }

    // What cannot be done in place: said, and the statements that would do
    // it left for whoever runs the script to look at.
    void manual(int change, Phase phase, const QString &message, const QStringList &sql = {})
    {
        hazard(change, HazardType::Manual, message);
        QString commented;
        for (const QString &statement : sql) {
            for (const QString &line : statement.split(QLatin1Char('\n')))
                commented += QStringLiteral("-- ") + line + QLatin1Char('\n');
        }
        if (commented.isEmpty())
            commented = QStringLiteral("-- ") + message;
        emit(change, phase, commented.trimmed());
    }

    // Adds a Changed entry the first time something differs.
    class Lazy
    {
    public:
        Lazy(Builder &builder, ObjectType type, QString name)
            : m_builder(builder), m_type(type), m_name(std::move(name))
        { }

        int operator()(const QString &detail)
        {
            if (m_change < 0)
                m_change = m_builder.add(ChangeKind::Changed, m_type, m_name);
            QString &details = m_builder.m_changes[m_change].details;
            if (!detail.isEmpty() && !details.split(QStringLiteral(", ")).contains(detail))
                details += (details.isEmpty() ? QString() : QStringLiteral(", ")) + detail;
            return m_change;
        }

        bool changed() const { return m_change >= 0; }

    private:
        Builder &m_builder;
        ObjectType m_type;
        QString m_name;
        int m_change = -1;
    };

    // Schemas and extensions.

    void schemas()
    {
        for (const Schema &schema : m_reference.schemas) {
            const Schema *old = byName(m_target.schemas, schema.name);
            if (!old) {
                const int change = add(ChangeKind::Added, ObjectType::Schema, schema.name);
                emit(change, CreateSchemas,
                     QStringLiteral("CREATE SCHEMA %1;").arg(q(schema.name)));
                if (!schema.comment.isEmpty())
                    emit(change, Comments,
                         commentOn(QStringLiteral("SCHEMA ") + q(schema.name), schema.comment));
            } else if (old->comment != schema.comment) {
                const int change = add(ChangeKind::Changed, ObjectType::Schema, schema.name,
                                       QStringLiteral("comment"));
                emit(change, Comments,
                     commentOn(QStringLiteral("SCHEMA ") + q(schema.name), schema.comment));
            }
        }
        for (const Schema &schema : m_target.schemas) {
            if (byName(m_reference.schemas, schema.name))
                continue;
            const int change = add(ChangeKind::Removed, ObjectType::Schema, schema.name);
            emit(change, DropSchemas, QStringLiteral("DROP SCHEMA %1;").arg(q(schema.name)));
        }
    }

    // Extensions belong to the database rather than to a schema; only those
    // in the schemas compared, or in the system's, are.
    bool compared(const Extension &extension) const
    {
        return extension.schema.startsWith(QLatin1String("pg_"))
            || byName(m_target.schemas, extension.schema)
            || byName(m_reference.schemas, extension.schema);
    }

    void extensions()
    {
        for (const Extension &extension : m_reference.extensions) {
            if (!compared(extension))
                continue;
            const Extension *old = byName(m_target.extensions, extension.name);
            if (!old) {
                const int change = add(ChangeKind::Added, ObjectType::Extension, extension.name);
                emit(change, CreateExtensions,
                     QStringLiteral("CREATE EXTENSION IF NOT EXISTS %1 WITH SCHEMA %2 VERSION %3;")
                         .arg(q(extension.name), q(extension.schema), literal(extension.version)));
                continue;
            }
            Lazy change(*this, ObjectType::Extension, extension.name);
            if (old->version != extension.version)
                emit(change(QStringLiteral("version %1 → %2").arg(old->version, extension.version)),
                     CreateExtensions,
                     QStringLiteral("ALTER EXTENSION %1 UPDATE TO %2;")
                         .arg(q(extension.name), literal(extension.version)));
            if (old->schema != extension.schema)
                emit(change(QStringLiteral("schema")), CreateExtensions,
                     QStringLiteral("ALTER EXTENSION %1 SET SCHEMA %2;")
                         .arg(q(extension.name), q(extension.schema)));
        }
        for (const Extension &extension : m_target.extensions) {
            if (!compared(extension) || byName(m_reference.extensions, extension.name))
                continue;
            const int change = add(ChangeKind::Removed, ObjectType::Extension, extension.name);
            hazard(change, HazardType::DataLoss,
                   QStringLiteral("Drops whatever the extension made, data included."));
            emit(change, DropExtensions,
                 QStringLiteral("DROP EXTENSION %1;").arg(q(extension.name)));
        }
    }

    // Types and domains.

    static QString typeKeyword(const UserType &type)
    {
        return type.kind == 'd' ? QStringLiteral("DOMAIN") : QStringLiteral("TYPE");
    }

    static ObjectType typeType(const UserType &type)
    {
        return type.kind == 'd' ? ObjectType::Domain : ObjectType::Type;
    }

    static QString createType(const UserType &type)
    {
        const QString name = qq(type.schema, type.name);
        switch (type.kind) {
        case 'e': {
            QStringList labels;
            for (const QString &label : type.labels)
                labels << literal(label);
            return QStringLiteral("CREATE TYPE %1 AS ENUM (%2);")
                .arg(name, labels.join(QStringLiteral(", ")));
        }
        case 'r':
            return QStringLiteral("CREATE TYPE %1 AS RANGE (SUBTYPE = %2);")
                .arg(name, type.baseType);
        case 'c': {
            QStringList attributes;
            for (const Column &attribute : type.attributes)
                attributes << q(attribute.name) + QLatin1Char(' ') + attribute.type;
            return QStringLiteral("CREATE TYPE %1 AS (%2);")
                .arg(name, attributes.join(QStringLiteral(", ")));
        }
        default: {
            QString sql = QStringLiteral("CREATE DOMAIN %1 AS %2").arg(name, type.baseType);
            if (!type.defaultValue.isEmpty())
                sql += QStringLiteral(" DEFAULT ") + type.defaultValue;
            if (type.notNull)
                sql += QStringLiteral(" NOT NULL");
            for (const Constraint &constraint : type.constraints)
                sql += QStringLiteral(" CONSTRAINT %1 %2")
                           .arg(q(constraint.name), constraint.definition);
            return sql + QLatin1Char(';');
        }
        }
    }

    void types()
    {
        // Ranges and domains may be of enums, composites of any of them.
        static const QString Order = QStringLiteral("erdc");
        std::vector<const UserType *> created;
        for (const UserType &type : m_reference.types) {
            const auto old = std::ranges::find_if(m_target.types, [&](const UserType &t) {
                return t.qualifiedName() == type.qualifiedName();
            });
            if (old == m_target.types.end())
                created.push_back(&type);
            else
                alterType(*old, type);
        }
        std::ranges::stable_sort(
            created, {}, [](const UserType *t) { return Order.indexOf(QLatin1Char(t->kind)); });
        for (const UserType *type : created) {
            const int change = add(ChangeKind::Added, typeType(*type), type->qualifiedName());
            emit(change, CreateTypes, createType(*type));
            if (!type->comment.isEmpty())
                emit(change, Comments,
                     commentOn(typeKeyword(*type) + QLatin1Char(' ') + qq(type->schema, type->name),
                               type->comment));
        }
        for (const UserType &type : m_target.types) {
            const bool kept = std::ranges::any_of(m_reference.types, [&](const UserType &t) {
                return t.qualifiedName() == type.qualifiedName();
            });
            if (kept)
                continue;
            const int change = add(ChangeKind::Removed, typeType(type), type.qualifiedName());
            emit(change, DropTypes,
                 QStringLiteral("DROP %1 %2;").arg(typeKeyword(type), qq(type.schema, type.name)));
        }
    }

    void alterType(const UserType &from, const UserType &to)
    {
        const QString name = qq(to.schema, to.name);
        Lazy change(*this, typeType(to), to.qualifiedName());
        if (from.kind != to.kind) {
            manual(change(QStringLiteral("kind")), CreateTypes,
                   QStringLiteral("The type is of another kind: drop it and make it again, along "
                                  "with whatever uses it."),
                   {QStringLiteral("DROP %1 %2;").arg(typeKeyword(from), name), createType(to)});
            return;
        }
        switch (to.kind) {
        case 'e':
            alterEnum(from, to, change);
            break;
        case 'r':
            if (from.baseType != to.baseType)
                manual(change(QStringLiteral("subtype")), CreateTypes,
                       QStringLiteral("A range's subtype cannot be changed: drop the range and "
                                      "make it again."),
                       {QStringLiteral("DROP TYPE %1;").arg(name), createType(to)});
            break;
        case 'c':
            for (const Column &attribute : from.attributes) {
                if (!byName(to.attributes, attribute.name))
                    emit(change(QStringLiteral("attributes")), AlterTypes,
                         QStringLiteral("ALTER TYPE %1 DROP ATTRIBUTE %2;")
                             .arg(name, q(attribute.name)));
            }
            for (const Column &attribute : to.attributes) {
                const Column *old = byName(from.attributes, attribute.name);
                if (!old)
                    emit(change(QStringLiteral("attributes")), AlterTypes,
                         QStringLiteral("ALTER TYPE %1 ADD ATTRIBUTE %2 %3;")
                             .arg(name, q(attribute.name), attribute.type));
                else if (old->type != attribute.type)
                    emit(change(QStringLiteral("attributes")), AlterTypes,
                         QStringLiteral("ALTER TYPE %1 ALTER ATTRIBUTE %2 TYPE %3;")
                             .arg(name, q(attribute.name), attribute.type));
            }
            break;
        default:
            alterDomain(from, to, change);
            break;
        }
        if (from.comment != to.comment)
            emit(change(QStringLiteral("comment")), Comments,
                 commentOn(typeKeyword(to) + QLatin1Char(' ') + name, to.comment));
    }

    void alterEnum(const UserType &from, const UserType &to, Lazy &change)
    {
        const QString name = qq(to.schema, to.name);
        // Values can only be added; the old ones have to stay in their order.
        qsizetype next = 0;
        for (const QString &label : to.labels) {
            if (next < from.labels.size() && from.labels[next] == label)
                ++next;
        }
        if (next < from.labels.size()) {
            QStringList gone;
            for (const QString &label : from.labels) {
                if (!to.labels.contains(label))
                    gone << label;
            }
            manual(change(QStringLiteral("values")), AlterTypes,
                   gone.isEmpty()
                       ? QStringLiteral("The values are in another order, which cannot be changed "
                                        "in place.")
                       : QStringLiteral("Values cannot be removed from an enum in place: %1.")
                             .arg(gone.join(QStringLiteral(", "))));
            return;
        }
        for (qsizetype i = 0; i < to.labels.size(); ++i) {
            if (from.labels.contains(to.labels[i]))
                continue;
            const QString where = i > 0
                ? QStringLiteral(" AFTER ") + literal(to.labels[i - 1])
                : (to.labels.size() > 1 ? QStringLiteral(" BEFORE ") + literal(to.labels[1])
                                        : QString());
            m_pending.push_back({AddEnumValues,
                                 0,
                                 {QStringLiteral("ALTER TYPE %1 ADD VALUE IF NOT EXISTS %2%3;")
                                      .arg(name, literal(to.labels[i]), where),
                                  change(QStringLiteral("values")), true, true}});
        }
    }

    void alterDomain(const UserType &from, const UserType &to, Lazy &change)
    {
        const QString name = qq(to.schema, to.name);
        if (from.baseType != to.baseType) {
            manual(change(QStringLiteral("type")), CreateTypes,
                   QStringLiteral("A domain's type cannot be changed: drop the domain and make it "
                                  "again, along with whatever uses it."),
                   {QStringLiteral("DROP DOMAIN %1;").arg(name), createType(to)});
            return;
        }
        if (from.defaultValue != to.defaultValue)
            emit(
                change(QStringLiteral("default")), AlterTypes,
                to.defaultValue.isEmpty()
                    ? QStringLiteral("ALTER DOMAIN %1 DROP DEFAULT;").arg(name)
                    : QStringLiteral("ALTER DOMAIN %1 SET DEFAULT %2;").arg(name, to.defaultValue));
        if (from.notNull != to.notNull) {
            const int c = change(QStringLiteral("not null"));
            emit(c, AlterTypes,
                 QStringLiteral("ALTER DOMAIN %1 %2 NOT NULL;")
                     .arg(name, to.notNull ? QStringLiteral("SET") : QStringLiteral("DROP")));
            if (to.notNull)
                hazard(c, HazardType::Blocking,
                       QStringLiteral("Every column of the domain is checked for nulls."));
        }
        for (const Constraint &constraint : from.constraints) {
            const Constraint *now = byName(to.constraints, constraint.name);
            if (!now || now->definition != constraint.definition)
                emit(change(QStringLiteral("constraints")), AlterTypes,
                     QStringLiteral("ALTER DOMAIN %1 DROP CONSTRAINT %2;")
                         .arg(name, q(constraint.name)));
        }
        for (const Constraint &constraint : to.constraints) {
            const Constraint *old = byName(from.constraints, constraint.name);
            if (old && old->definition == constraint.definition)
                continue;
            const int c = change(QStringLiteral("constraints"));
            emit(c, AlterTypes,
                 QStringLiteral("ALTER DOMAIN %1 ADD CONSTRAINT %2 %3;")
                     .arg(name, q(constraint.name), constraint.definition));
            hazard(c, HazardType::Blocking,
                   QStringLiteral("Every column of the domain is checked against %1.")
                       .arg(constraint.name));
        }
    }

    // Sequences.

    // The column a serial's sequence belongs to, quoted, or empty.
    static QString owner(const Model &model, const QString &ownedBy)
    {
        for (const Relation &relation : model.relations) {
            const QString prefix = relation.qualifiedName() + QLatin1Char('.');
            if (!ownedBy.startsWith(prefix))
                continue;
            if (const Column *column = relation.column(ownedBy.mid(prefix.size())))
                return qq(relation) + QLatin1Char('.') + q(column->name);
        }
        return {};
    }

    static const Column *ownerColumn(const Model &model, const QString &ownedBy)
    {
        for (const Relation &relation : model.relations) {
            const QString prefix = relation.qualifiedName() + QLatin1Char('.');
            if (ownedBy.startsWith(prefix)) {
                if (const Column *column = relation.column(ownedBy.mid(prefix.size())))
                    return column;
            }
        }
        return nullptr;
    }

    // An identity column's sequence is the column's: made and dropped with it.
    static bool isIdentity(const Model &model, const Sequence &sequence)
    {
        const Column *column = ownerColumn(model, sequence.ownedBy);
        return column && !column->identity.isEmpty();
    }

    static const Sequence *findSequence(const Model &model, const QString &qualified)
    {
        const auto it = std::ranges::find_if(model.sequences, [&](const Sequence &s) {
            return s.qualifiedName() == qualified && !isIdentity(model, s);
        });
        return it == model.sequences.end() ? nullptr : &*it;
    }

    static QString sequenceOptions(const Sequence &s)
    {
        return QStringLiteral(
                   "AS %1 INCREMENT BY %2 MINVALUE %3 MAXVALUE %4 START WITH %5 CACHE %6 %7")
            .arg(s.type)
            .arg(s.increment)
            .arg(s.minimum)
            .arg(s.maximum)
            .arg(s.start)
            .arg(s.cache)
            .arg(s.cycle ? QStringLiteral("CYCLE") : QStringLiteral("NO CYCLE"));
    }

    void sequences()
    {
        for (const Sequence &sequence : m_reference.sequences) {
            if (isIdentity(m_reference, sequence))
                continue;
            const QString name = qq(sequence.schema, sequence.name);
            const Sequence *old = findSequence(m_target, sequence.qualifiedName());
            if (!old) {
                const int change
                    = add(ChangeKind::Added, ObjectType::Sequence, sequence.qualifiedName());
                emit(change, CreateSequences,
                     QStringLiteral("CREATE SEQUENCE %1 %2;").arg(name, sequenceOptions(sequence)));
                if (const QString column = owner(m_reference, sequence.ownedBy); !column.isEmpty())
                    emit(change, AlterSequences,
                         QStringLiteral("ALTER SEQUENCE %1 OWNED BY %2;").arg(name, column));
                if (!sequence.comment.isEmpty())
                    emit(change, Comments,
                         commentOn(QStringLiteral("SEQUENCE ") + name, sequence.comment));
                continue;
            }
            Lazy change(*this, ObjectType::Sequence, sequence.qualifiedName());
            QStringList clauses;
            if (old->type != sequence.type)
                clauses << QStringLiteral("AS ") + sequence.type;
            if (old->increment != sequence.increment)
                clauses << QStringLiteral("INCREMENT BY %1").arg(sequence.increment);
            if (old->minimum != sequence.minimum)
                clauses << QStringLiteral("MINVALUE %1").arg(sequence.minimum);
            if (old->maximum != sequence.maximum)
                clauses << QStringLiteral("MAXVALUE %1").arg(sequence.maximum);
            if (old->start != sequence.start)
                clauses << QStringLiteral("START WITH %1").arg(sequence.start);
            if (old->cache != sequence.cache)
                clauses << QStringLiteral("CACHE %1").arg(sequence.cache);
            if (old->cycle != sequence.cycle)
                clauses << (sequence.cycle ? QStringLiteral("CYCLE") : QStringLiteral("NO CYCLE"));
            if (!clauses.isEmpty())
                emit(change(QStringLiteral("options")), CreateSequences,
                     QStringLiteral("ALTER SEQUENCE %1 %2;")
                         .arg(name, clauses.join(QLatin1Char(' '))));
            if (old->ownedBy != sequence.ownedBy) {
                const QString column = owner(m_reference, sequence.ownedBy);
                emit(change(QStringLiteral("owned by")), AlterSequences,
                     QStringLiteral("ALTER SEQUENCE %1 OWNED BY %2;")
                         .arg(name, column.isEmpty() ? QStringLiteral("NONE") : column));
            }
            if (old->comment != sequence.comment)
                emit(change(QStringLiteral("comment")), Comments,
                     commentOn(QStringLiteral("SEQUENCE ") + name, sequence.comment));
        }
        for (const Sequence &sequence : m_target.sequences) {
            if (isIdentity(m_target, sequence)
                || findSequence(m_reference, sequence.qualifiedName()))
                continue;
            // A serial's sequence goes with its column, when that goes.
            if (!sequence.ownedBy.isEmpty() && !ownerColumn(m_reference, sequence.ownedBy))
                continue;
            const int change
                = add(ChangeKind::Removed, ObjectType::Sequence, sequence.qualifiedName());
            emit(change, DropSequences,
                 QStringLiteral("DROP SEQUENCE %1;").arg(qq(sequence.schema, sequence.name)));
        }
    }

    // Functions and procedures.

    void routines()
    {
        QHash<QString, const Routine *> old;
        for (const Routine &routine : m_target.routines)
            old.insert(routineKey(routine), &routine);
        QSet<QString> kept;
        for (const Routine &routine : m_reference.routines) {
            const QString key = routineKey(routine);
            const ObjectType type
                = routine.kind == 'p' ? ObjectType::Procedure : ObjectType::Function;
            const QString what = routineKeyword(routine) + QLatin1Char(' ') + routineName(routine);
            const Routine *before = old.value(key);
            if (!before) {
                const int change = add(ChangeKind::Added, type, key);
                if (routine.kind == 'a')
                    manual(change, CreateRoutines,
                           QStringLiteral("Aggregates are not compared: make it by hand."));
                else
                    emit(change, CreateRoutines, withSemicolon(routine.definition));
                if (!routine.comment.isEmpty())
                    emit(change, Comments, commentOn(what, routine.comment));
                continue;
            }
            kept.insert(key);
            Lazy change(*this, type, key);
            if (before->definition != routine.definition) {
                const int c = change(QStringLiteral("definition"));
                // CREATE OR REPLACE keeps what depends on the routine, but
                // cannot change what it returns or what it is.
                if (before->result != routine.result || before->kind != routine.kind)
                    emit(c, CreateRoutines,
                         QStringLiteral("DROP %1 %2;")
                             .arg(routineKeyword(*before), routineName(*before)));
                emit(c, CreateRoutines, withSemicolon(routine.definition));
            }
            if (before->comment != routine.comment)
                emit(change(QStringLiteral("comment")), Comments, commentOn(what, routine.comment));
        }
        for (const Routine &routine : m_target.routines) {
            const QString key = routineKey(routine);
            if (kept.contains(key))
                continue;
            const int change
                = add(ChangeKind::Removed,
                      routine.kind == 'p' ? ObjectType::Procedure : ObjectType::Function, key);
            emit(change, DropRoutines,
                 QStringLiteral("DROP %1 %2;").arg(routineKeyword(routine), routineName(routine)));
        }
    }

    // Tables and views.

    static const Relation *findRelation(const Model &model, const QString &qualified)
    {
        const auto it = std::ranges::find_if(model.relations, [&](const Relation &r) {
            return r.kind != 'f' && r.qualifiedName() == qualified;
        });
        return it == model.relations.end() ? nullptr : &*it;
    }

    // How many relations a view reads through other views: views are made
    // in this order, and dropped in the other.
    static int depth(const Model &model, const Relation &relation, QSet<Oid> &seen)
    {
        if (!relation.isView() || seen.contains(relation.oid))
            return 0;
        seen.insert(relation.oid);
        int deepest = 0;
        for (const Dependency &dependency : model.dependencies) {
            if (dependency.view != relation.oid)
                continue;
            if (const Relation *used = model.relation(dependency.relation))
                deepest = std::max(deepest, 1 + depth(model, *used, seen));
        }
        return deepest;
    }

    static int depth(const Model &model, const Relation &relation)
    {
        QSet<Oid> seen;
        return depth(model, relation, seen);
    }

    // Partitions of partitions come after their parents.
    static int partitionDepth(const Model &model, const Relation &relation)
    {
        int depth = 0;
        for (const Relation *r = &relation; r && r->isPartition() && depth < 32; ++depth)
            r = model.relation(r->parentSchema, r->parentName);
        return depth;
    }

    void relations()
    {
        // Tables first, as views made again because a table's column
        // changes have to know.
        std::vector<std::pair<const Relation *, const Relation *>> views;
        for (const Relation &relation : m_reference.relations) {
            if (relation.kind == 'f')
                continue;
            const Relation *old = findRelation(m_target, relation.qualifiedName());
            if (old && old->isView() == relation.isView() && !relation.isView()) {
                alterTable(*old, relation);
            } else if (old && old->isView() && relation.isView()) {
                views.emplace_back(old, &relation);
            } else if (old) {
                // A table that becomes a view or the other way round.
                const int change = add(ChangeKind::Changed, relationType(relation.kind),
                                       relation.qualifiedName(), QStringLiteral("kind"));
                if (old->isView()) {
                    m_disturbed.insert(old->oid);
                    dropView(change, *old);
                } else {
                    dropTable(change, *old);
                }
                createRelation(change, relation);
            } else {
                createRelation(
                    add(ChangeKind::Added, relationType(relation.kind), relation.qualifiedName()),
                    relation);
            }
        }
        for (const Relation &relation : m_target.relations) {
            if (relation.kind == 'f' || findRelation(m_reference, relation.qualifiedName()))
                continue;
            m_disturbed.insert(relation.oid);
            // A partition goes with its table.
            if (relation.isPartition()
                && !findRelation(m_reference,
                                 relation.parentSchema + QLatin1Char('.') + relation.parentName)
                && findRelation(m_target,
                                relation.parentSchema + QLatin1Char('.') + relation.parentName))
                continue;
            const int change
                = add(ChangeKind::Removed, relationType(relation.kind), relation.qualifiedName());
            if (relation.isView())
                dropView(change, relation);
            else
                dropTable(change, relation);
        }
        compareViews(views);
    }

    // Views reading this one first.
    void dropView(int change, const Relation &view)
    {
        emit(change, DropViews,
             QStringLiteral("DROP %1 %2;").arg(relationKeyword(view.kind), qq(view)), true,
             -depth(m_target, view));
    }

    void dropTable(int change, const Relation &relation)
    {
        hazard(change, HazardType::DataLoss, QStringLiteral("Drops the table and its rows."));
        // Foreign keys first, in case the table they point to goes too.
        for (const Constraint &constraint : relation.constraints) {
            if (constraint.type == 'f' && constraint.referencedOid != relation.oid)
                emit(change, DropForeignKeys,
                     QStringLiteral("ALTER TABLE %1 DROP CONSTRAINT %2;")
                         .arg(qq(relation), q(constraint.name)));
        }
        emit(change, DropTables, QStringLiteral("DROP TABLE %1;").arg(qq(relation)));
    }

    void createRelation(int change, const Relation &relation)
    {
        if (relation.isView()) {
            createView(change, relation);
            return;
        }
        const QString name = qq(relation);
        QString sql;
        if (relation.isPartition()) {
            sql = QStringLiteral("CREATE TABLE %1 PARTITION OF %2 %3")
                      .arg(name, qq(relation.parentSchema, relation.parentName),
                           relation.partitionBound);
        } else {
            QStringList columns;
            for (const Column &column : relation.columns)
                columns << QStringLiteral("    ") + columnDefinition(column);
            sql = QStringLiteral("CREATE %1TABLE %2 (\n%3\n)")
                      .arg(relation.unlogged ? QStringLiteral("UNLOGGED ") : QString(), name,
                           columns.join(QStringLiteral(",\n")));
        }
        if (!relation.partitionKey.isEmpty())
            sql += QStringLiteral(" PARTITION BY ") + relation.partitionKey;
        if (!relation.options.isEmpty())
            sql += QStringLiteral(" WITH (%1)").arg(relation.options);
        sql += QLatin1Char(';');
        if (relation.isPartition())
            emit(change, CreatePartitions, sql, true, partitionDepth(m_reference, relation));
        else
            emit(change, CreateTables, sql);
        if (relation.rowSecurity)
            emit(change, AlterTables,
                 QStringLiteral("ALTER TABLE %1 ENABLE ROW LEVEL SECURITY;").arg(name));

        // A partition gets its keys, indexes and triggers from its table.
        if (!relation.isPartition()) {
            for (const Constraint &constraint : relation.constraints) {
                if (constraint.type != 't')
                    addConstraint(change, relation, constraint, false);
            }
            const QSet<QString> owned = constraintIndexes(relation);
            for (const Index &index : relation.indexes) {
                if (!owned.contains(index.name))
                    createIndex(change, relation, index, false);
            }
            for (const Trigger &trigger : relation.triggers)
                createTrigger(change, relation, trigger);
            for (const Policy &policy : relation.policies)
                emit(change, CreatePolicies, createPolicy(relation, policy));
        }
        relationComments(change, nullptr, relation);
    }

    void createView(int change, const Relation &view)
    {
        const QString name = qq(view);
        QString sql = QStringLiteral("CREATE %1 %2").arg(relationKeyword(view.kind), name);
        if (!view.options.isEmpty())
            sql += QStringLiteral(" WITH (%1)").arg(view.options);
        sql += QStringLiteral(" AS\n") + withoutSemicolon(view.definition) + QLatin1Char(';');
        emit(change, CreateViews, sql, true, depth(m_reference, view));
        for (const Index &index : view.indexes)
            createIndex(change, view, index, false);
        for (const Trigger &trigger : view.triggers)
            createTrigger(change, view, trigger);
        relationComments(change, nullptr, view);
    }

    // Comments of a relation and its parts that differ from old's, or all
    // of them for a new one.
    void relationComments(int change, const Relation *old, const Relation &relation,
                          Lazy *lazy = nullptr)
    {
        const auto commented
            = [&](const QString &detail) { return lazy ? (*lazy)(detail) : change; };
        const QString name = qq(relation);
        if ((old ? old->comment : QString()) != relation.comment)
            emit(commented(QStringLiteral("comment")), Comments,
                 commentOn(relationKeyword(relation.kind) + QLatin1Char(' ') + name,
                           relation.comment));
        for (const Column &column : relation.columns) {
            const Column *before = old ? old->column(column.name) : nullptr;
            // A new column of an existing table says so itself.
            if (old && !before)
                continue;
            if ((before ? before->comment : QString()) != column.comment)
                emit(commented(QStringLiteral("comment")), Comments,
                     commentOn(QStringLiteral("COLUMN %1.%2").arg(name, q(column.name)),
                               column.comment));
        }
    }

    void addConstraint(int change, const Relation &relation, const Constraint &constraint,
                       bool existing)
    {
        const QString name = qq(relation);
        const bool foreign = constraint.type == 'f';
        const Phase phase = foreign ? AddForeignKeys : AddConstraints;
        // On rows already there, NOT VALID adds it at once, and checking
        // them afterwards lets writes go on meanwhile. Partitioned tables
        // cannot do that.
        const bool deferValidation = existing && (foreign || constraint.type == 'c')
            && relation.kind != 'p' && !constraint.definition.contains(QLatin1String("NOT VALID"));
        emit(change, phase,
             QStringLiteral("ALTER TABLE %1 ADD CONSTRAINT %2 %3%4;")
                 .arg(name, q(constraint.name), constraint.definition,
                      deferValidation ? QStringLiteral(" NOT VALID") : QString()));
        if (deferValidation)
            emit(change, phase,
                 QStringLiteral("ALTER TABLE %1 VALIDATE CONSTRAINT %2;")
                     .arg(name, q(constraint.name)));
        else if (existing && constraint.type != 'f' && constraint.type != 'c')
            hazard(change, HazardType::Blocking,
                   QStringLiteral("%1 builds an index while writes to %2 wait.")
                       .arg(constraint.name, relation.qualifiedName()));
        if (!constraint.comment.isEmpty())
            emit(change, Comments,
                 commentOn(QStringLiteral("CONSTRAINT %1 ON %2").arg(q(constraint.name), name),
                           constraint.comment));
    }

    void dropConstraint(int change, const Relation &relation, const Constraint &constraint)
    {
        emit(change, constraint.type == 'f' ? DropForeignKeys : DropConstraints,
             QStringLiteral("ALTER TABLE %1 DROP CONSTRAINT %2;")
                 .arg(qq(relation), q(constraint.name)));
    }

    bool concurrently(const Relation &relation, bool existing) const
    {
        return existing && m_options.concurrentIndexes && relation.kind != 'p';
    }

    void createIndex(int change, const Relation &relation, const Index &index, bool existing)
    {
        // Of a partitioned table, the server says ON ONLY: an index of its
        // own that the partitions' are attached to later. Without ONLY, the
        // partitions get theirs at once.
        QString sql = index.definition;
        sql.replace(QLatin1String(" ON ONLY "), QLatin1String(" ON "));
        const bool concurrent = concurrently(relation, existing);
        if (concurrent)
            sql.replace(sql.indexOf(QLatin1String("INDEX ")), 6,
                        QStringLiteral("INDEX CONCURRENTLY "));
        else if (existing)
            hazard(change, HazardType::Blocking,
                   QStringLiteral("Building %1 makes writes to %2 wait.")
                       .arg(index.name, relation.qualifiedName()));
        emit(change, relation.isView() ? CreateViewIndexes : CreateIndexes, withSemicolon(sql),
             !concurrent);
        if (!index.comment.isEmpty())
            emit(change, Comments,
                 commentOn(QStringLiteral("INDEX ") + qq(relation.schema, index.name),
                           index.comment));
    }

    void dropIndex(int change, const Relation &relation, const Index &index)
    {
        const bool concurrent = concurrently(relation, true);
        emit(change, DropIndexes,
             QStringLiteral("DROP INDEX %1%2;")
                 .arg(concurrent ? QStringLiteral("CONCURRENTLY ") : QString(),
                      qq(relation.schema, index.name)),
             !concurrent);
    }

    void createTrigger(int change, const Relation &relation, const Trigger &trigger)
    {
        emit(change, CreateTriggers, withSemicolon(trigger.definition));
        if (!trigger.enabled)
            emit(change, CreateTriggers,
                 QStringLiteral("ALTER TABLE %1 DISABLE TRIGGER %2;")
                     .arg(qq(relation), q(trigger.name)));
        if (!trigger.comment.isEmpty())
            emit(change, Comments,
                 commentOn(QStringLiteral("TRIGGER %1 ON %2").arg(q(trigger.name), qq(relation)),
                           trigger.comment));
    }

    void alterTable(const Relation &from, const Relation &to)
    {
        const QString name = qq(to);
        Lazy change(*this, ObjectType::Table, to.qualifiedName());
        if ((from.kind == 'p') != (to.kind == 'p') || from.partitionKey != to.partitionKey) {
            manual(change(QStringLiteral("partitioning")), AlterTables,
                   QStringLiteral("How the table is partitioned cannot be changed in place: make a "
                                  "new table and move the rows."));
        }
        if (from.unlogged != to.unlogged) {
            const int c
                = change(to.unlogged ? QStringLiteral("unlogged") : QStringLiteral("logged"));
            emit(c, AlterTables,
                 QStringLiteral("ALTER TABLE %1 SET %2;")
                     .arg(name,
                          to.unlogged ? QStringLiteral("UNLOGGED") : QStringLiteral("LOGGED")));
            hazard(c, HazardType::Rewrite, QStringLiteral("The table is rewritten."));
        }
        if (from.rowSecurity != to.rowSecurity)
            emit(change(QStringLiteral("row security")), AlterTables,
                 QStringLiteral("ALTER TABLE %1 %2 ROW LEVEL SECURITY;")
                     .arg(name,
                          to.rowSecurity ? QStringLiteral("ENABLE") : QStringLiteral("DISABLE")));
        if (from.options != to.options) {
            const QMap<QString, QString> before = parseOptions(from.options);
            const QMap<QString, QString> after = parseOptions(to.options);
            QStringList set;
            QStringList reset;
            for (auto it = after.cbegin(); it != after.cend(); ++it) {
                if (before.value(it.key()) != it.value())
                    set << it.key() + QLatin1Char('=') + it.value();
            }
            for (auto it = before.cbegin(); it != before.cend(); ++it) {
                if (!after.contains(it.key()))
                    reset << it.key();
            }
            if (!reset.isEmpty())
                emit(change(QStringLiteral("options")), AlterTables,
                     QStringLiteral("ALTER TABLE %1 RESET (%2);")
                         .arg(name, reset.join(QStringLiteral(", "))));
            if (!set.isEmpty())
                emit(change(QStringLiteral("options")), AlterTables,
                     QStringLiteral("ALTER TABLE %1 SET (%2);")
                         .arg(name, set.join(QStringLiteral(", "))));
        }
        if (from.parentSchema != to.parentSchema || from.parentName != to.parentName
            || from.partitionBound != to.partitionBound) {
            const int c = change(QStringLiteral("partition"));
            if (from.isPartition())
                emit(c, DetachPartitions,
                     QStringLiteral("ALTER TABLE %1 DETACH PARTITION %2;")
                         .arg(qq(from.parentSchema, from.parentName), name));
            if (to.isPartition()) {
                emit(c, AttachPartitions,
                     QStringLiteral("ALTER TABLE %1 ATTACH PARTITION %2 %3;")
                         .arg(qq(to.parentSchema, to.parentName), name, to.partitionBound));
                hazard(c, HazardType::Blocking,
                       QStringLiteral("The rows are checked against the partition's bounds."));
            }
        }
        relationComments(-1, &from, to, &change);

        // A partition's columns, keys, indexes and triggers are its table's.
        if (to.isPartition())
            return;
        alterColumns(from, to);
        alterParts(from, to);
    }

    void alterColumns(const Relation &from, const Relation &to)
    {
        const QString table = qq(to);
        for (const Column &column : from.columns) {
            if (to.column(column.name))
                continue;
            m_disturbed.insert(from.oid);
            const int change = add(ChangeKind::Removed, ObjectType::Column,
                                   from.qualifiedName() + QLatin1Char('.') + column.name);
            hazard(change, HazardType::DataLoss, QStringLiteral("The column's values are lost."));
            emit(change, DropColumns,
                 QStringLiteral("ALTER TABLE %1 DROP COLUMN %2;").arg(table, q(column.name)));
        }
        for (const Column &column : to.columns) {
            const QString columnName = to.qualifiedName() + QLatin1Char('.') + column.name;
            const Column *old = from.column(column.name);
            if (!old) {
                const int change = add(ChangeKind::Added, ObjectType::Column, columnName);
                emit(change, AlterTables,
                     QStringLiteral("ALTER TABLE %1 ADD COLUMN %2;")
                         .arg(table, columnDefinition(column)));
                if (!column.identity.isEmpty()
                    || (!column.generated.isEmpty() && !column.virtualGenerated))
                    hazard(change, HazardType::Rewrite,
                           QStringLiteral("Every row is written again to fill the column."));
                else if (column.notNull && column.defaultValue.isEmpty()
                         && column.generated.isEmpty())
                    hazard(
                        change, HazardType::Manual,
                        QStringLiteral("NOT NULL without a default fails if the table has rows."));
                if (!column.comment.isEmpty())
                    emit(change, Comments,
                         commentOn(QStringLiteral("COLUMN %1.%2").arg(table, q(column.name)),
                                   column.comment));
                continue;
            }
            alterColumn(to, *old, column);
        }
    }

    void alterColumn(const Relation &relation, const Column &from, const Column &to)
    {
        const QString alter
            = QStringLiteral("ALTER TABLE %1 ALTER COLUMN %2 ").arg(qq(relation), q(to.name));
        Lazy change(*this, ObjectType::Column,
                    relation.qualifiedName() + QLatin1Char('.') + to.name);
        if (from.generated != to.generated || from.virtualGenerated != to.virtualGenerated) {
            // Its values are computed, so nothing is lost in making it again.
            m_disturbed.insert(relationOid(m_target, relation));
            const int c = change(QStringLiteral("generated"));
            emit(c, AlterTables,
                 QStringLiteral("ALTER TABLE %1 DROP COLUMN %2;").arg(qq(relation), q(to.name)));
            emit(c, AlterTables,
                 QStringLiteral("ALTER TABLE %1 ADD COLUMN %2;")
                     .arg(qq(relation), columnDefinition(to)));
            if (to.generated.isEmpty())
                hazard(c, HazardType::DataLoss,
                       QStringLiteral("The column is made again, without its computed values."));
            else if (!to.virtualGenerated)
                hazard(c, HazardType::Rewrite, QStringLiteral("Every row is written again."));
            return;
        }
        const bool retype = from.type != to.type;
        if (!from.identity.isEmpty() && to.identity.isEmpty())
            emit(change(QStringLiteral("identity")), AlterTables,
                 alter + QStringLiteral("DROP IDENTITY;"));
        // The old default may not cast to the new type.
        if (!from.defaultValue.isEmpty() && (retype || to.defaultValue.isEmpty()))
            emit(change(QStringLiteral("default")), AlterTables,
                 alter + QStringLiteral("DROP DEFAULT;"));
        if (retype) {
            m_disturbed.insert(relationOid(m_target, relation));
            const int c = change(QStringLiteral("type %1 → %2").arg(from.type, to.type));
            emit(c, AlterTables,
                 alter + QStringLiteral("TYPE %1 USING %2::%1;").arg(to.type, q(to.name)));
            hazard(c, HazardType::Rewrite,
                   QStringLiteral("The table may be rewritten, and values that do not fit %1 fail.")
                       .arg(to.type));
        }
        if (from.notNull && !to.notNull)
            emit(change(QStringLiteral("not null")), AlterTables,
                 alter + QStringLiteral("DROP NOT NULL;"));
        if (!from.notNull && to.notNull) {
            const int c = change(QStringLiteral("not null"));
            emit(c, AlterTables, alter + QStringLiteral("SET NOT NULL;"));
            hazard(c, HazardType::Blocking,
                   QStringLiteral("The table is scanned for nulls while writes wait, and it fails "
                                  "if there are any."));
        }
        if (!to.defaultValue.isEmpty() && (retype || from.defaultValue != to.defaultValue))
            emit(change(QStringLiteral("default")), AlterTables,
                 alter + QStringLiteral("SET DEFAULT %1;").arg(to.defaultValue));
        if (from.identity.isEmpty() && !to.identity.isEmpty())
            emit(change(QStringLiteral("identity")), AlterTables,
                 alter + QStringLiteral("ADD GENERATED %1 AS IDENTITY;").arg(to.identity));
        else if (!from.identity.isEmpty() && !to.identity.isEmpty() && from.identity != to.identity)
            emit(change(QStringLiteral("identity")), AlterTables,
                 alter + QStringLiteral("SET GENERATED %1;").arg(to.identity));
    }

    static Oid relationOid(const Model &model, const Relation &relation)
    {
        const Relation *found = findRelation(model, relation.qualifiedName());
        return found ? found->oid : 0;
    }

    // Constraints, indexes, triggers and policies of a table in both.
    void alterParts(const Relation &from, const Relation &to)
    {
        const QString table = qq(to);
        const auto partName = [&](const QString &name) {
            return name + QStringLiteral(" on ") + to.qualifiedName();
        };

        // A constraint trigger is a trigger, compared as one.
        for (const Constraint &constraint : from.constraints) {
            if (constraint.type == 't' || byName(to.constraints, constraint.name))
                continue;
            dropConstraint(
                add(ChangeKind::Removed, ObjectType::Constraint, partName(constraint.name)), from,
                constraint);
        }
        for (const Constraint &constraint : to.constraints) {
            if (constraint.type == 't')
                continue;
            const Constraint *old = byName(from.constraints, constraint.name);
            if (!old) {
                addConstraint(
                    add(ChangeKind::Added, ObjectType::Constraint, partName(constraint.name)), to,
                    constraint, true);
                continue;
            }
            Lazy change(*this, ObjectType::Constraint, partName(constraint.name));
            if (old->definition != constraint.definition) {
                const int c = change(QStringLiteral("definition"));
                dropConstraint(c, from, *old);
                addConstraint(c, to, constraint, true);
            } else if (old->comment != constraint.comment) {
                emit(change(QStringLiteral("comment")), Comments,
                     commentOn(QStringLiteral("CONSTRAINT %1 ON %2").arg(q(constraint.name), table),
                               constraint.comment));
            }
        }

        const QSet<QString> fromOwned = constraintIndexes(from);
        const QSet<QString> toOwned = constraintIndexes(to);
        for (const Index &index : from.indexes) {
            if (fromOwned.contains(index.name) || toOwned.contains(index.name))
                continue;
            if (const Index *now = byName(to.indexes, index.name); now)
                continue;
            dropIndex(add(ChangeKind::Removed, ObjectType::Index, partName(index.name)), from,
                      index);
        }
        for (const Index &index : to.indexes) {
            if (toOwned.contains(index.name))
                continue;
            const Index *old
                = fromOwned.contains(index.name) ? nullptr : byName(from.indexes, index.name);
            if (!old) {
                createIndex(add(ChangeKind::Added, ObjectType::Index, partName(index.name)), to,
                            index, true);
                continue;
            }
            Lazy change(*this, ObjectType::Index, partName(index.name));
            if (old->definition != index.definition) {
                const int c = change(QStringLiteral("definition"));
                dropIndex(c, from, *old);
                createIndex(c, to, index, true);
            } else if (old->comment != index.comment) {
                emit(
                    change(QStringLiteral("comment")), Comments,
                    commentOn(QStringLiteral("INDEX ") + qq(to.schema, index.name), index.comment));
            }
        }

        alterTriggers(from, to);

        for (const Policy &policy : from.policies) {
            const Policy *now = byName(to.policies, policy.name);
            if (now && samePolicy(policy, *now))
                continue;
            const int change = now
                ? add(ChangeKind::Changed, ObjectType::Policy, partName(policy.name),
                      QStringLiteral("definition"))
                : add(ChangeKind::Removed, ObjectType::Policy, partName(policy.name));
            emit(change, DropPolicies,
                 QStringLiteral("DROP POLICY %1 ON %2;").arg(q(policy.name), table));
            if (now)
                emit(change, CreatePolicies, createPolicy(to, *now));
        }
        for (const Policy &policy : to.policies) {
            if (!byName(from.policies, policy.name))
                emit(add(ChangeKind::Added, ObjectType::Policy, partName(policy.name)),
                     CreatePolicies, createPolicy(to, policy));
        }
    }

    void alterTriggers(const Relation &from, const Relation &to)
    {
        const QString table = qq(to);
        const auto partName = [&](const QString &name) {
            return name + QStringLiteral(" on ") + to.qualifiedName();
        };
        for (const Trigger &trigger : from.triggers) {
            if (!byName(to.triggers, trigger.name))
                emit(add(ChangeKind::Removed, ObjectType::Trigger, partName(trigger.name)),
                     DropTriggers,
                     QStringLiteral("DROP TRIGGER %1 ON %2;").arg(q(trigger.name), table));
        }
        for (const Trigger &trigger : to.triggers) {
            const Trigger *old = byName(from.triggers, trigger.name);
            if (!old) {
                createTrigger(add(ChangeKind::Added, ObjectType::Trigger, partName(trigger.name)),
                              to, trigger);
                continue;
            }
            Lazy change(*this, ObjectType::Trigger, partName(trigger.name));
            if (old->definition != trigger.definition) {
                const int c = change(QStringLiteral("definition"));
                emit(c, DropTriggers,
                     QStringLiteral("DROP TRIGGER %1 ON %2;").arg(q(trigger.name), table));
                emit(c, CreateTriggers, withSemicolon(trigger.definition));
                if (!trigger.enabled)
                    emit(c, CreateTriggers,
                         QStringLiteral("ALTER TABLE %1 DISABLE TRIGGER %2;")
                             .arg(table, q(trigger.name)));
            } else if (old->enabled != trigger.enabled) {
                emit(
                    change(trigger.enabled ? QStringLiteral("enabled")
                                           : QStringLiteral("disabled")),
                    CreateTriggers,
                    QStringLiteral("ALTER TABLE %1 %2 TRIGGER %3;")
                        .arg(table,
                             trigger.enabled ? QStringLiteral("ENABLE") : QStringLiteral("DISABLE"),
                             q(trigger.name)));
            }
            if (old->comment != trigger.comment)
                emit(change(QStringLiteral("comment")), Comments,
                     commentOn(QStringLiteral("TRIGGER %1 ON %2").arg(q(trigger.name), table),
                               trigger.comment));
        }
    }

    // Views in both. One whose definition differs is made again, and so is
    // every view reading from it, or from a table whose columns change
    // under it: the server would not let either be altered.
    void compareViews(const std::vector<std::pair<const Relation *, const Relation *>> &views)
    {
        QSet<Oid> recreate;
        for (const auto &[from, to] : views) {
            if (from->kind != to->kind || from->definition != to->definition
                || from->options != to->options)
                recreate.insert(from->oid);
        }
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto &[from, to] : views) {
                if (recreate.contains(from->oid))
                    continue;
                for (const Dependency &dependency : m_target.dependencies) {
                    if (dependency.view == from->oid
                        && (recreate.contains(dependency.relation)
                            || m_disturbed.contains(dependency.relation))) {
                        recreate.insert(from->oid);
                        grew = true;
                        break;
                    }
                }
            }
        }
        for (const auto &[from, to] : views) {
            if (recreate.contains(from->oid)) {
                const bool own = from->kind != to->kind || from->definition != to->definition
                    || from->options != to->options;
                const int change
                    = add(ChangeKind::Changed, relationType(to->kind), to->qualifiedName(),
                          own ? QStringLiteral("definition")
                              : QStringLiteral("made again for what it reads"));
                dropView(change, *from);
                createView(change, *to);
                continue;
            }
            // Only indexes, triggers and comments can differ.
            Lazy change(*this, relationType(to->kind), to->qualifiedName());
            for (const Index &index : from->indexes) {
                const Index *now = byName(to->indexes, index.name);
                if (!now || now->definition != index.definition)
                    dropIndex(change(QStringLiteral("indexes")), *from, index);
            }
            for (const Index &index : to->indexes) {
                const Index *old = byName(from->indexes, index.name);
                if (!old || old->definition != index.definition)
                    createIndex(change(QStringLiteral("indexes")), *to, index, true);
            }
            if (from->triggers.size() != to->triggers.size()
                || !std::ranges::equal(from->triggers, to->triggers, {}, &Trigger::definition,
                                       &Trigger::definition)) {
                for (const Trigger &trigger : from->triggers)
                    emit(change(QStringLiteral("triggers")), DropTriggers,
                         QStringLiteral("DROP TRIGGER %1 ON %2;").arg(q(trigger.name), qq(*from)));
                for (const Trigger &trigger : to->triggers)
                    createTrigger(change(QStringLiteral("triggers")), *to, trigger);
            }
            relationComments(-1, from, *to, &change);
        }
    }

    const Model &m_target;
    const Model &m_reference;
    Options m_options;
    std::vector<Change> m_changes;
    std::vector<Pending> m_pending;
    // Relations whose columns change or which go: views reading them are
    // made again.
    QSet<Oid> m_disturbed;
};

} // namespace

bool Diff::transactional() const
{
    return std::ranges::all_of(statements,
                               [](const Statement &s) { return s.early || s.transactional; });
}

Diff compare(const Model &target, const Model &reference, const Options &options)
{
    return Builder(target, reference, options).run();
}

QString kindName(ChangeKind kind)
{
    switch (kind) {
    case ChangeKind::Added:
        return QStringLiteral("added");
    case ChangeKind::Removed:
        return QStringLiteral("removed");
    case ChangeKind::Changed:
        break;
    }
    return QStringLiteral("changed");
}

QString typeName(ObjectType type)
{
    switch (type) {
    case ObjectType::Schema:
        return QStringLiteral("schema");
    case ObjectType::Extension:
        return QStringLiteral("extension");
    case ObjectType::Type:
        return QStringLiteral("type");
    case ObjectType::Domain:
        return QStringLiteral("domain");
    case ObjectType::Sequence:
        return QStringLiteral("sequence");
    case ObjectType::Function:
        return QStringLiteral("function");
    case ObjectType::Procedure:
        return QStringLiteral("procedure");
    case ObjectType::Table:
        return QStringLiteral("table");
    case ObjectType::View:
        return QStringLiteral("view");
    case ObjectType::MaterializedView:
        return QStringLiteral("materialized view");
    case ObjectType::Column:
        return QStringLiteral("column");
    case ObjectType::Constraint:
        return QStringLiteral("constraint");
    case ObjectType::Index:
        return QStringLiteral("index");
    case ObjectType::Trigger:
        return QStringLiteral("trigger");
    case ObjectType::Policy:
        break;
    }
    return QStringLiteral("policy");
}

QString hazardName(HazardType type)
{
    switch (type) {
    case HazardType::DataLoss:
        return QStringLiteral("data loss");
    case HazardType::Rewrite:
        return QStringLiteral("table rewrite");
    case HazardType::Blocking:
        return QStringLiteral("blocks writes");
    case HazardType::Manual:
        break;
    }
    return QStringLiteral("needs attention");
}

QString script(const Diff &diff, const ScriptOptions &options,
               std::vector<std::vector<std::pair<int, int>>> *lines)
{
    if (lines)
        lines->assign(diff.changes.size(), {});
    QStringList out;
    out << QStringLiteral("-- Makes %1 like %2.")
               .arg(options.targetName.isEmpty() ? QStringLiteral("the target")
                                                 : options.targetName,
                    options.referenceName.isEmpty() ? QStringLiteral("the reference")
                                                    : options.referenceName);
    if (diff.isEmpty()) {
        out << QStringLiteral("-- The schemas are the same: there is nothing to do.");
        return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }
    out << QStringLiteral("-- Read it before running it: the hazards are marked.");
    const bool transaction = options.transaction && diff.transactional();
    if (!options.transaction || transaction)
        out << QString();
    else
        out << QStringLiteral("-- Not in a transaction: CONCURRENTLY cannot run in one.")
            << QString();
    std::vector<bool> introduced(diff.changes.size(), false);
    int previous = -1;
    const auto write = [&](const Statement &statement) {
        const Change &change = diff.changes[statement.change];
        if (statement.change != previous) {
            out << QString();
            if (lines)
                (*lines)[statement.change].push_back({int(out.size()), int(out.size())});
            QString heading = QStringLiteral("-- %1 %2 %3")
                                  .arg(kindName(change.kind), typeName(change.type), change.name);
            if (!change.details.isEmpty())
                heading += QStringLiteral(": ") + change.details;
            out << heading;
            if (!introduced[statement.change]) {
                for (const Hazard &hazard : change.hazards)
                    out << QStringLiteral("-- Hazard, %1: %2")
                               .arg(hazardName(hazard.type), hazard.message);
                introduced[statement.change] = true;
            }
            previous = statement.change;
        }
        out << statement.sql.split(QLatin1Char('\n'));
        if (lines)
            (*lines)[statement.change].back().second = int(out.size()) - 1;
    };

    // Routines are made before the tables their bodies may read.
    if (transaction) {
        for (const Statement &statement : diff.statements) {
            if (statement.early)
                write(statement);
        }
        if (previous >= 0)
            out << QString();
        previous = -1;
        out << QStringLiteral("BEGIN;") << QStringLiteral("SET LOCAL check_function_bodies = off;");
    } else {
        out << QStringLiteral("SET check_function_bodies = off;");
    }
    for (const Statement &statement : diff.statements) {
        if (!transaction || !statement.early)
            write(statement);
    }
    if (transaction)
        out << QString() << QStringLiteral("COMMIT;");
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QByteArray databaseListQuery()
{
    return "SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY 1";
}

void loadModel(pg::QueryRunner *runner, const SchemaFilter &filter, QObject *context,
               std::function<void(const Model &model, const QString &error)> done)
{
    docgen::loadModel(runner, filter, context, std::move(done), true);
}

} // namespace slonisko::schemadiff
