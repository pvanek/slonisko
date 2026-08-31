// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"
#include "pg/RowStore.h"

#include <QByteArray>
#include <QByteArrayList>
#include <QString>

#include <optional>
#include <vector>

namespace slonisko::catalog {

// The table a result's rows can be written back to.
struct EditTarget
{
    QString schema;
    QString table;
    // Per result column: its name in the table, or empty when it is not
    // one of the table's columns (computed, or from elsewhere).
    std::vector<QString> columns;
    std::vector<bool> readOnly; // Generated and GENERATED ALWAYS identity columns.
    std::vector<int> key; // Result columns holding the primary key, in key order.
    QString reason; // Why the result is not editable, if it is not.

    bool editable() const { return !key.empty(); }
    bool writable(int column) const
    {
        return editable() && column < int(columns.size()) && !columns[std::size_t(column)].isEmpty()
            && !readOnly[std::size_t(column)];
    }
};

// The one table a result's columns come from, or 0 with a reason when
// there is none or more than one.
Oid sourceTable(const pg::RowStore &rows, QString *reason = nullptr);

// SQL describing a table for editing: its name, kind, primary key and
// columns. Run it, then pass the results to editTarget().
QByteArray editTargetQuery(Oid table);
EditTarget editTarget(const pg::RowStore &rows, const std::vector<pg::Result> &description);

// A change to one row of an editable result.
struct RowEdit
{
    enum class Kind { Update, Insert, Delete };
    using Value = std::optional<QByteArray>; // nullopt is NULL.

    Kind kind = Kind::Update;
    std::vector<Value> key; // The row's key before the change (Update, Delete).
    std::vector<std::pair<int, Value>> values; // Result column and new value (Update, Insert).
};

// One statement per edit, each meant to change exactly one row. Values go
// in as literals in PostgreSQL's text form, which it casts to the column's
// type.
QByteArrayList dmlStatements(const EditTarget &target, const std::vector<RowEdit> &edits);

// Whether a statement's command tag says it changed exactly one row.
bool changedOneRow(const QByteArray &commandTag);

} // namespace slonisko::catalog
