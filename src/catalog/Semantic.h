// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace slonisko::catalog {

// What a stretch of SQL text turned out to be, beyond what the lexer sees.
struct SemanticSpan
{
    enum class Kind : std::uint8_t {
        Relation, // A table, view or similar found in the catalog.
        Cte,
        Column,
        Function,
        UnknownRelation, // Queried, but not in the catalog nor created by the script.
        UnknownColumn, // alias.column where the alias's table has no such column.
        // The body of a function in another language, like plpython3u:
        ForeignText,
        ForeignKeyword,
        ForeignString,
        ForeignComment,
        ForeignNumber,
    };

    Kind kind = Kind::Relation;
    qsizetype offset = 0; // In bytes, in the script.
    qsizetype length = 0;
    QString detail; // For a tooltip, like a column's type.

    qsizetype end() const { return offset + length; }
};

// Semantic spans for the statements of a script that overlap bytes
// [from, to) (to < 0: to the end), sorted by offset. Parses each statement
// and resolves its names against the snapshot; statements that do not parse
// get what can be found from their tokens, without "unknown" marks. Without
// a snapshot, only other languages' function bodies are found. Thread-safe.
std::vector<SemanticSpan> analyzeScript(const QByteArray &script, const Snapshot *snapshot,
                                        qsizetype from = 0, qsizetype to = -1);

} // namespace slonisko::catalog
