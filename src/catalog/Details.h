// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace slonisko::pg {
class Result;
}

namespace slonisko::catalog {

// What one object is: its own properties, lists that belong to it (columns,
// indexes, …) and, where there is one, the SQL that defines it.
struct DetailTable
{
    QString title;
    QStringList columns;
    std::vector<QStringList> rows;
};

struct ObjectDetail
{
    QString title; // "public.orders"
    QString subtitle; // "table", "view", "extension"
    DetailTable properties; // Two columns: property and value.
    std::vector<DetailTable> tables;
    QString definition; // A view's query, a function's source, ...
};

// Whether an object of this kind has a details page at all.
bool hasDetails(ObjectKind kind);

// The queries for one object, to run in order; their results go to
// parseDetail() in the same order.
std::vector<QByteArray> detailQueries(ObjectKind kind, Oid oid);
ObjectDetail parseDetail(ObjectKind kind, const std::vector<pg::Result> &results);

} // namespace slonisko::catalog
