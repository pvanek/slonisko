// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Splitter.h"

#include <pg_query.h>

namespace slonisko::sql {

SplitResult splitStatements(const QByteArray &utf8Script)
{
    SplitResult out;

    // QByteArray is always NUL-terminated.
    PgQuerySplitResult r = pg_query_split_with_scanner(utf8Script.constData());
    if (r.error) {
        out.error = QString::fromUtf8(r.error->message);
    } else {
        out.statements.reserve(static_cast<std::size_t>(r.n_stmts));
        for (int i = 0; i < r.n_stmts; ++i) {
            const qsizetype offset = r.stmts[i]->stmt_location;
            qsizetype length = r.stmts[i]->stmt_len;
            // PostgreSQL convention: 0 means "to the end of the string".
            if (length == 0)
                length = utf8Script.size() - offset;
            out.statements.push_back({offset, length});
        }
    }
    pg_query_free_split_result(r);
    return out;
}

} // namespace slonisko::sql
