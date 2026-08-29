// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>

#include <vector>

namespace slonisko::sql {

// A piece of a script, as a span of the UTF-8 source in bytes.
struct StatementSpan
{
    enum class Kind {
        // An SQL statement. The span starts at the first token and ends after
        // the last one: surrounding whitespace, leading and trailing comments
        // and the terminating semicolon are not included. It may contain \;,
        // which psql replaces with ; to send several statements as one query.
        Sql,
        // A psql meta-command such as \set or \connect, with its arguments,
        // but without trailing whitespace or a \\ separator.
        PsqlCommand,
        // Inline data for the preceding COPY ... FROM STDIN or \copy ... from
        // stdin: whole lines, including their line ends, up to the \. line.
        CopyData,
    };

    qsizetype offset = 0;
    qsizetype length = 0;
    Kind kind = Kind::Sql;
    // Sql: ended with a semicolon or a query-executing meta-command like \g.
    // False when the script or a meta-command like \echo cuts the statement
    // short, including in the middle of a string, comment or block.
    // CopyData: ended with a \. line rather than the end of the script.
    // PsqlCommand: always true.
    bool terminated = false;
};

// Splits a script into statements the way psql does, without parsing it.
// Semicolons inside quotes, dollar quotes, comments, parentheses and
// BEGIN ATOMIC ... END function bodies do not end a statement. Recognises
// psql meta-commands and inline COPY data, so pg_dump output splits
// correctly. Assumes standard_conforming_strings is on. Never fails:
// incomplete input yields an unterminated last span, so it is safe to call
// on text being edited. Spans are returned in script order.
std::vector<StatementSpan> splitStatements(const QByteArray &utf8Script);

} // namespace slonisko::sql
