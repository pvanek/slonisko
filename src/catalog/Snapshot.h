// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace slonisko::catalog {

using Oid = unsigned int;

struct Relation
{
    Oid oid = 0;
    QString schema;
    QString name;
    char kind = 'r'; // pg_class.relkind
};

// Immutable view of one database's catalog. The loader builds a new snapshot
// and swaps the pointer; readers (completion threads) never lock.
struct Snapshot
{
    int serverVersion = 0;
    QStringList searchPath;
    std::vector<Relation> relations;

    // Resolves an unqualified name via search_path, or a qualified one directly.
    const Relation *findRelation(const QString &schema, const QString &name) const;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

} // namespace slonisko::catalog
