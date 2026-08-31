// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>
#include <vector>

namespace slonisko::sql {

enum class TokenKind : std::uint8_t {
    Whitespace,
    Comment,
    Keyword,
    Identifier,
    QuotedIdentifier,
    String,
    DollarDelimiter, // $tag$ opening or closing a dollar-quoted string or body.
    DollarString, // The contents of a dollar-quoted string.
    Number,
    Operator,
    Punctuation, // ( ) [ ] , ; .
    Parameter, // $1
    PsqlVariable, // :name, :'name', :"name"
    PsqlCommand, // \set and the like, to the end of the line.
    Unknown,
};

struct Token
{
    TokenKind kind = TokenKind::Unknown;
    qsizetype offset = 0; // In bytes, within the text given to tokenize().
    qsizetype length = 0;
    // Inside a function or DO body, which is lexed as SQL.
    bool inBody = false;

    qsizetype end() const { return offset + length; }
};

// Where the lexer is, so lexing can resume there, e.g. at the start of the
// next line.
struct LexState
{
    enum class Mode : std::uint8_t {
        Code,
        BlockComment,
        String, // '...', also B'', X'', N'' and U&''.
        EscapeString, // E'...', with backslash escapes.
        QuotedIdentifier,
        DollarString,
    };
    // After AS or DO, a dollar quote opens a body lexed as SQL. After DO
    // LANGUAGE, the language name comes first.
    enum class Expect : std::uint8_t { Nothing, Body, LanguageThenBody };

    Mode mode = Mode::Code;
    int commentDepth = 0; // For BlockComment: they nest.
    QByteArray tag; // For DollarString: the delimiter, like "$$" or "$body$".
    QByteArray bodyTag; // Inside a body: the delimiter that ends it.
    Expect expect = Expect::Nothing;

    bool operator==(const LexState &) const = default;
};

// Splits text into tokens, starting in state and leaving the state at the
// end of the text in it. Lexing a text in pieces, like line by line, gives
// the same tokens as lexing it whole, except that tokens spanning pieces are
// cut at the piece boundaries. Never fails: anything unknown becomes an
// Unknown token.
std::vector<Token> tokenize(QByteArrayView text, LexState &state);

inline std::vector<Token> tokenize(QByteArrayView text)
{
    LexState state;
    return tokenize(text, state);
}

// Whether a token is code, not whitespace or a comment.
inline bool isSignificant(TokenKind kind)
{
    return kind != TokenKind::Whitespace && kind != TokenKind::Comment;
}

} // namespace slonisko::sql
