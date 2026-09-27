// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"

#include <QByteArray>
#include <QString>

#include <vector>

namespace slonisko::catalog {

struct CompletionItem
{
    enum class Kind {
        Keyword,
        Schema,
        Table,
        View,
        MaterializedView,
        ForeignTable,
        Cte, // A WITH query, subquery or function in FROM.
        Alias, // A name in scope that stands for a relation.
        Column,
        Function,
        Type,
        Snippet, // A short word standing for a piece of SQL, like "sf".
    };

    Kind kind = Kind::Keyword;
    QString label; // What the list shows.
    QString insertText; // What goes into the text, quoted if needed.
    QString detail; // Like a column's type, or a relation's schema.
    // Where the caret goes once the text is in, counted in characters from
    // its start; -1 leaves it at the end. Snippets put it where the name of
    // the table belongs.
    int caret = -1;
    int score = 0;
};

struct Completion
{
    enum class Context {
        None, // In a comment or string: nothing to offer.
        Keyword, // After a complete expression or name, like "FROM t |".
        Relation, // After FROM, JOIN, UPDATE, INTO, ...
        Column, // In a select list, WHERE, ON, ...
        Qualified, // After "x.": columns of x, or objects in schema x.
        Type,
        Function,
    };

    Context context = Context::None;
    // What to replace, in bytes within the statement: the word at the
    // cursor, or an empty range at it.
    qsizetype replaceFrom = 0;
    qsizetype replaceTo = 0;
    QString prefix; // The part of the word before the cursor.
    std::vector<CompletionItem> items; // Best first.
};

// Suggestions for the cursor at byte offset cursor in one SQL statement.
// Uses PostgreSQL's parser on the statement with the word at the cursor
// replaced by a placeholder, and token-based guesses when even that does not
// parse. Thread-safe: runs off the UI thread.
Completion complete(const QByteArray &statement, qsizetype cursor, const Snapshot &snapshot);

// How well a candidate matches what was typed: 0 for no match, higher for
// better (prefix, then word starts like "cnm" for customer_name, then any
// subsequence). Case-insensitive.
int fuzzyScore(const QString &typed, const QString &candidate);

// Which letters of candidate fuzzyScore() matched, in order, for showing
// what the suggestion has in common with what was typed. Empty when they do
// not match at all.
std::vector<qsizetype> fuzzyMatchPositions(const QString &typed, const QString &candidate);

} // namespace slonisko::catalog
