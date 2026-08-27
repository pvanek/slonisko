// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Snapshot.h"

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

} // namespace slonisko::catalog
