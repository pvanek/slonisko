// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Result.h"

#include <QByteArray>
#include <QByteArrayView>

#include <optional>
#include <vector>

namespace slonisko::catalog {

// How a single value from a result reads: the value viewer formats and
// highlights by it.
enum class ValueSyntax {
    Text,
    Json,
    Array // PostgreSQL's array literal, {a,"b c",NULL}.
};

// By the column's type; a text column holding a JSON object or array, or
// a value of a type not known here that reads as an array, by its value.
ValueSyntax guessSyntax(Oid type, QByteArrayView value);

struct ValueToken
{
    enum Kind {
        Punctuation, // Braces, brackets, commas, colons; an array's dimensions.
        Key, // A JSON object's member name.
        String,
        Number,
        Literal, // true, false and null in JSON, NULL in an array.
        Error, // What does not belong where it is.
    };
    Kind kind;
    qsizetype start; // In bytes of the UTF-8 text.
    qsizetype length;
};

// Whitespace between tokens is not one. Nothing is left out otherwise, so
// even text that is not JSON gets tokens to show it with, errors included.
std::vector<ValueToken> tokenizeJson(QByteArrayView text);
std::vector<ValueToken> tokenizeArray(QByteArrayView text);
std::vector<ValueToken> tokenize(ValueSyntax syntax, QByteArrayView text);

bool isValidJson(QByteArrayView text);

// One member or element per line, indented by four spaces as jsonb_pretty()
// does, but with the members in the order they came, and numbers as they
// were written. Nullopt for text that is not JSON.
std::optional<QByteArray> prettyJson(QByteArrayView text);
// Each element of the outermost array on a line of its own, and of every
// array that holds arrays; an innermost array of a multidimensional one, a
// row of a matrix, stays on one line. Nullopt when the braces or quotes do
// not match.
std::optional<QByteArray> prettyArray(QByteArrayView text);
std::optional<QByteArray> pretty(ValueSyntax syntax, QByteArrayView text);

} // namespace slonisko::catalog
