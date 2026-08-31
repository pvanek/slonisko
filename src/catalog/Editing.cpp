// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Editing.h"

#include "catalog/Scope.h"

#include <QHash>
#include <QSet>

namespace slonisko::catalog {

namespace {

QByteArray identifier(const QString &name)
{
    return scope::quoted(name).toUtf8();
}

QByteArray literal(const RowEdit::Value &value)
{
    if (!value)
        return "NULL";
    // Assumes standard_conforming_strings, like the rest of Slonisko.
    QByteArray escaped = *value;
    escaped.replace('\'', "''");
    return '\'' + escaped + '\'';
}

} // namespace

Oid sourceTable(const pg::RowStore &rows, QString *reason)
{
    QSet<Oid> tables;
    for (int c = 0; c < rows.columnCount(); ++c) {
        if (rows.column(c).table != 0)
            tables.insert(rows.column(c).table);
    }
    if (tables.size() == 1)
        return *tables.begin();
    if (reason)
        *reason = tables.isEmpty() ? QStringLiteral("The result does not come from a table.")
                                   : QStringLiteral("The result has columns from several tables.");
    return 0;
}

QByteArray editTargetQuery(Oid table)
{
    const QByteArray oid = QByteArray::number(table);
    return "SELECT n.nspname, c.relname, c.relkind, "
           "coalesce((SELECT i.indkey::text FROM pg_index i WHERE i.indrelid = c.oid AND "
           "i.indisprimary), '') "
           "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = "
        + oid
        + ";"
          "SELECT a.attnum, a.attname, a.attgenerated <> '' OR a.attidentity = 'a' "
          "FROM pg_attribute a WHERE a.attrelid = "
        + oid + " AND a.attnum > 0 AND NOT a.attisdropped;";
}

EditTarget editTarget(const pg::RowStore &rows, const std::vector<pg::Result> &description)
{
    EditTarget target;
    const Oid table = sourceTable(rows, &target.reason);
    if (table == 0)
        return target;
    if (description.size() != 2 || description[0].rowCount() != 1) {
        target.reason = QStringLiteral("The table could not be looked up.");
        return target;
    }
    const pg::Result &info = description[0];
    target.schema = QString::fromUtf8(info.value(0, 0));
    target.table = QString::fromUtf8(info.value(0, 1));
    const QByteArray kind = info.value(0, 2);
    if (kind != "r" && kind != "p") {
        target.reason = QStringLiteral("%1.%2 is not a table.").arg(target.schema, target.table);
        return target;
    }

    // The table's columns by attribute number.
    const pg::Result &attributes = description[1];
    QHash<int, std::pair<QString, bool>> byNumber;
    for (int row = 0; row < attributes.rowCount(); ++row)
        byNumber.insert(
            attributes.value(row, 0).toInt(),
            {QString::fromUtf8(attributes.value(row, 1)), attributes.value(row, 2) == "t"});

    target.columns.resize(std::size_t(rows.columnCount()));
    target.readOnly.resize(std::size_t(rows.columnCount()));
    QHash<int, int> resultColumnOf; // Attribute number to result column.
    for (int c = 0; c < rows.columnCount(); ++c) {
        const pg::RowStore::Column &column = rows.column(c);
        if (column.table != table || !byNumber.contains(column.tableColumn))
            continue;
        const auto &[name, generated] = byNumber.value(column.tableColumn);
        target.columns[std::size_t(c)] = name;
        target.readOnly[std::size_t(c)] = generated;
        if (!resultColumnOf.contains(column.tableColumn))
            resultColumnOf.insert(column.tableColumn, c);
    }

    const QByteArray key = info.value(0, 3).trimmed();
    if (key.isEmpty()) {
        target.reason
            = QStringLiteral("%1.%2 has no primary key.").arg(target.schema, target.table);
        return target;
    }
    QStringList missing;
    std::vector<int> keyColumns;
    for (const QByteArray &number : key.split(' ')) {
        const int attnum = number.toInt();
        if (resultColumnOf.contains(attnum))
            keyColumns.push_back(resultColumnOf.value(attnum));
        else
            missing << byNumber.value(attnum).first;
    }
    if (!missing.isEmpty()) {
        target.reason = QStringLiteral("The result lacks the primary key column(s) %1.")
                            .arg(missing.join(QStringLiteral(", ")));
        return target;
    }
    target.key = std::move(keyColumns);
    return target;
}

QByteArrayList dmlStatements(const EditTarget &target, const std::vector<RowEdit> &edits)
{
    const QByteArray table = identifier(target.schema) + '.' + identifier(target.table);
    auto where = [&](const RowEdit &e) {
        QByteArrayList conditions;
        for (std::size_t i = 0; i < target.key.size() && i < e.key.size(); ++i)
            conditions << identifier(target.columns[std::size_t(target.key[i])]) + " = "
                    + literal(e.key[i]);
        return " WHERE " + conditions.join(" AND ");
    };

    QByteArrayList out;
    for (const RowEdit &e : edits) {
        switch (e.kind) {
        case RowEdit::Kind::Update: {
            QByteArrayList sets;
            for (const auto &[column, value] : e.values)
                sets << identifier(target.columns[std::size_t(column)]) + " = " + literal(value);
            if (!sets.isEmpty())
                out << "UPDATE " + table + " SET " + sets.join(", ") + where(e);
            break;
        }
        case RowEdit::Kind::Insert: {
            QByteArrayList names, values;
            for (const auto &[column, value] : e.values) {
                names << identifier(target.columns[std::size_t(column)]);
                values << literal(value);
            }
            out << (names.isEmpty() ? "INSERT INTO " + table + " DEFAULT VALUES"
                                    : "INSERT INTO " + table + " (" + names.join(", ")
                            + ") VALUES (" + values.join(", ") + ")");
            break;
        }
        case RowEdit::Kind::Delete:
            out << "DELETE FROM " + table + where(e);
            break;
        }
    }
    return out;
}

bool changedOneRow(const QByteArray &commandTag)
{
    return commandTag == "UPDATE 1" || commandTag == "DELETE 1" || commandTag == "INSERT 0 1";
}

} // namespace slonisko::catalog
