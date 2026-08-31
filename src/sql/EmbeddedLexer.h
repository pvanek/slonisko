// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <vector>

namespace slonisko::sql {

// Languages of function bodies other than SQL and PL/pgSQL, highlighted
// roughly: keywords, strings, comments and numbers.
enum class EmbeddedLanguage : std::uint8_t {
    Sql, // sql and plpgsql: lexed as SQL, not by this lexer.
    Python,
    Perl,
    Tcl,
    JavaScript,
    Lua,
    R,
    Shell,
    Other, // Unknown, or C and internal, whose "body" is a symbol name.
};

// The language of a PostgreSQL procedural language name, like plpython3u.
EmbeddedLanguage embeddedLanguage(const QString &pgLanguage);

struct EmbeddedToken
{
    enum class Kind : std::uint8_t { Keyword, String, Comment, Number };
    Kind kind;
    qsizetype offset = 0;
    qsizetype length = 0;
};

// Tokens of a body worth highlighting; everything else is plain text.
std::vector<EmbeddedToken> tokenizeEmbedded(QByteArrayView body, EmbeddedLanguage language);

} // namespace slonisko::sql
