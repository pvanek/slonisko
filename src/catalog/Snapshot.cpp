// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Snapshot.h"

#include "pg/Result.h"

#include <QHash>

#include <algorithm>

namespace slonisko::catalog {

const Relation *Snapshot::findRelation(const QString &schema, const QString &name) const
{
    auto lookup = [&](const QString &s) -> const Relation * {
        auto it = std::find_if(relations.begin(), relations.end(),
                               [&](const Relation &r) { return r.schema == s && r.name == name; });
        return it == relations.end() ? nullptr : &*it;
    };

    if (!schema.isEmpty())
        return lookup(schema);

    for (const QString &s : searchPath) {
        if (const Relation *r = lookup(s))
            return r;
    }
    return nullptr;
}

QByteArray snapshotQuery()
{
    // Temporary schemas are left out: their tables are only visible to the
    // session that created them, which is not the one loading this.
    return "SELECT current_schemas(true);"

           "SELECT n.nspname FROM pg_namespace n "
           "WHERE n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY 1;"

           "SELECT c.oid, n.nspname, c.relname, c.relkind FROM pg_class c "
           "JOIN pg_namespace n ON n.oid = c.relnamespace "
           "WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f') "
           "AND n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY 2, 3;"

           "SELECT a.attrelid, a.attname, format_type(a.atttypid, a.atttypmod) "
           "FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid "
           "JOIN pg_namespace n ON n.oid = c.relnamespace "
           "WHERE a.attnum > 0 AND NOT a.attisdropped AND c.relkind IN ('r', 'p', 'v', 'm', 'f') "
           "AND n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY a.attrelid, a.attnum;"

           "SELECT n.nspname, p.proname, pg_get_function_identity_arguments(p.oid), "
           "pg_get_function_result(p.oid), p.prokind FROM pg_proc p "
           "JOIN pg_namespace n ON n.oid = p.pronamespace "
           "WHERE n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY 1, 2;"

           // Leaves out array types and the row types of tables.
           "SELECT n.nspname, format_type(t.oid, NULL) FROM pg_type t "
           "JOIN pg_namespace n ON n.oid = t.typnamespace "
           "WHERE t.typtype IN ('b', 'c', 'd', 'e', 'r', 'm') AND t.typisdefined "
           "AND NOT EXISTS (SELECT FROM pg_type e WHERE e.oid = t.typelem AND e.typarray = t.oid) "
           "AND (t.typrelid = 0 OR (SELECT c.relkind FROM pg_class c WHERE c.oid = t.typrelid) = "
           "'c') "
           "AND n.nspname !~ '^pg_(toast|temp_|toast_temp_)' ORDER BY 1, 2;";
}

QStringList parseTextArray(const QString &array)
{
    QStringList out;
    if (array.size() < 2 || array.front() != QLatin1Char('{') || array.back() != QLatin1Char('}'))
        return out;
    QString current;
    bool quoted = false;
    bool any = false;
    for (qsizetype i = 1; i < array.size() - 1; ++i) {
        const QChar c = array[i];
        if (quoted) {
            if (c == QLatin1Char('\\') && i + 1 < array.size() - 1)
                current += array[++i];
            else if (c == QLatin1Char('"'))
                quoted = false;
            else
                current += c;
        } else if (c == QLatin1Char('"')) {
            quoted = true;
            any = true;
        } else if (c == QLatin1Char(',')) {
            out << current;
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (any || !current.isEmpty())
        out << current;
    return out;
}

namespace {

char firstChar(const QByteArray &value, char fallback)
{
    return value.isEmpty() ? fallback : value.front();
}

} // namespace

SnapshotPtr parseSnapshot(const std::vector<pg::Result> &results, int serverVersion)
{
    if (results.size() != 6 || std::ranges::any_of(results, [](const pg::Result &r) {
            return r.status() != PGRES_TUPLES_OK;
        }))
        return nullptr;

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->serverVersion = serverVersion;
    auto text = [](const pg::Result &r, int row, int column) {
        return QString::fromUtf8(r.value(row, column));
    };

    if (results[0].rowCount() > 0)
        snapshot->searchPath = parseTextArray(text(results[0], 0, 0));

    const pg::Result &schemas = results[1];
    for (int row = 0; row < schemas.rowCount(); ++row)
        snapshot->schemas << text(schemas, row, 0);

    const pg::Result &relations = results[2];
    QHash<Oid, std::size_t> byOid;
    snapshot->relations.reserve(std::size_t(relations.rowCount()));
    for (int row = 0; row < relations.rowCount(); ++row) {
        Relation r;
        r.oid = relations.value(row, 0).toUInt();
        r.schema = text(relations, row, 1);
        r.name = text(relations, row, 2);
        r.kind = firstChar(relations.value(row, 3), 'r');
        byOid.insert(r.oid, snapshot->relations.size());
        snapshot->relations.push_back(std::move(r));
    }

    const pg::Result &columns = results[3];
    for (int row = 0; row < columns.rowCount(); ++row) {
        const auto it = byOid.constFind(columns.value(row, 0).toUInt());
        if (it != byOid.cend())
            snapshot->relations[*it].columns.push_back(
                {text(columns, row, 1), text(columns, row, 2)});
    }

    const pg::Result &functions = results[4];
    snapshot->functions.reserve(std::size_t(functions.rowCount()));
    for (int row = 0; row < functions.rowCount(); ++row) {
        snapshot->functions.push_back({text(functions, row, 0), text(functions, row, 1),
                                       text(functions, row, 2), text(functions, row, 3),
                                       firstChar(functions.value(row, 4), 'f')});
    }

    const pg::Result &types = results[5];
    for (int row = 0; row < types.rowCount(); ++row)
        snapshot->types.push_back({text(types, row, 0), text(types, row, 1)});

    return snapshot;
}

} // namespace slonisko::catalog
