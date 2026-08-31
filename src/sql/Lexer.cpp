// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Lexer.h"

#include "sql/Keywords.h"

#include <cstring>

namespace slonisko::sql {

namespace {

using Mode = LexState::Mode;
using Expect = LexState::Expect;

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isHexDigit(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Bytes >= 0x80 are parts of UTF-8 sequences, which PostgreSQL allows in names.
bool isIdentStart(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}

bool isIdentCont(char c)
{
    return isIdentStart(c) || isDigit(c) || c == '$';
}

// PostgreSQL's operator characters. Any run of them is one operator, so
// user-defined operators like @>, ->> or <-> need no list.
bool isOperatorChar(char c)
{
    return c != '\0' && std::strchr("+-*/<>=~!@#%^&|`?", c) != nullptr;
}

bool isWord(QByteArrayView word, const char *keyword)
{
    return word.compare(QByteArrayView(keyword), Qt::CaseInsensitive) == 0;
}

class Lexer
{
public:
    Lexer(QByteArrayView text, LexState &state) : m_s(text), m_n(text.size()), m_state(state) { }

    std::vector<Token> run()
    {
        qsizetype i = 0;
        while (i < m_n)
            i = m_state.mode == Mode::Code ? code(i) : resume(i);
        return std::move(m_tokens);
    }

private:
    char peek(qsizetype i) const { return i < m_n ? m_s[i] : '\0'; }

    void emit(TokenKind kind, qsizetype from, qsizetype to)
    {
        if (to <= from)
            return;
        const bool inBody = !m_state.bodyTag.isEmpty() && kind != TokenKind::DollarDelimiter;
        m_tokens.push_back({kind, from, to - from, inBody});
    }

    // Where the enclosing body ends, or the end of the text: nothing inside
    // a body, not even a string, reaches past its closing delimiter.
    qsizetype limit(qsizetype from) const
    {
        if (m_state.bodyTag.isEmpty())
            return m_n;
        const qsizetype close = m_s.indexOf(QByteArrayView(m_state.bodyTag), from);
        return close < 0 ? m_n : close;
    }

    // Continues a construct left open by the previous piece of text.
    qsizetype resume(qsizetype i)
    {
        switch (m_state.mode) {
        case Mode::BlockComment:
            return blockComment(i, i);
        case Mode::String:
            return quoted(i, i, '\'', false, TokenKind::String);
        case Mode::EscapeString:
            return quoted(i, i, '\'', true, TokenKind::String);
        case Mode::QuotedIdentifier:
            return quoted(i, i, '"', false, TokenKind::QuotedIdentifier);
        case Mode::DollarString:
            return dollarString(i);
        case Mode::Code:
            break;
        }
        return code(i);
    }

    qsizetype code(qsizetype i)
    {
        const char c = m_s[i];

        if (!m_state.bodyTag.isEmpty()
            && m_s.sliced(i).startsWith(QByteArrayView(m_state.bodyTag))) {
            const qsizetype end = i + m_state.bodyTag.size();
            m_state.bodyTag.clear();
            m_state.expect = Expect::Nothing;
            emit(TokenKind::DollarDelimiter, i, end);
            return end;
        }

        if (isSpace(c)) {
            qsizetype end = i + 1;
            while (end < m_n && isSpace(m_s[end]))
                ++end;
            emit(TokenKind::Whitespace, i, end);
            return end;
        }
        if (c == '-' && peek(i + 1) == '-') {
            qsizetype end = i + 2;
            while (end < limit(i) && m_s[end] != '\n' && m_s[end] != '\r')
                ++end;
            emit(TokenKind::Comment, i, end);
            return end;
        }
        if (c == '/' && peek(i + 1) == '*') {
            m_state.mode = Mode::BlockComment;
            m_state.commentDepth = 1;
            return blockComment(i, i + 2);
        }

        // Anything else is code, which ends waiting for a body unless it is
        // the word that set it up.
        const Expect expect = m_state.expect;
        m_state.expect = Expect::Nothing;

        if (c == '\'') {
            m_state.mode = Mode::String;
            return quoted(i, i + 1, '\'', false, TokenKind::String);
        }
        if (c == '"') {
            m_state.mode = Mode::QuotedIdentifier;
            return quoted(i, i + 1, '"', false, TokenKind::QuotedIdentifier);
        }
        if (c == '$')
            return dollar(i, expect);
        if (isIdentStart(c))
            return word(i, expect);
        // .5 is a number, but in 1..10 the dots are punctuation.
        if (isDigit(c) || (c == '.' && isDigit(peek(i + 1)) && !(i > 0 && m_s[i - 1] == '.')))
            return number(i);
        if (isOperatorChar(c))
            return operatorRun(i);
        if (c == ':')
            return colon(i);
        if (c == '\\') {
            qsizetype end = i + 1;
            while (end < m_n && m_s[end] != '\n' && m_s[end] != '\r')
                ++end;
            emit(TokenKind::PsqlCommand, i, end);
            return end;
        }
        if (std::strchr("()[],;.", c)) {
            emit(TokenKind::Punctuation, i, i + 1);
            return i + 1;
        }
        emit(TokenKind::Unknown, i, i + 1);
        return i + 1;
    }

    qsizetype blockComment(qsizetype start, qsizetype i)
    {
        const qsizetype stop = limit(i);
        while (i < stop) {
            if (m_s[i] == '/' && peek(i + 1) == '*') {
                ++m_state.commentDepth;
                i += 2;
            } else if (m_s[i] == '*' && peek(i + 1) == '/') {
                i += 2;
                if (--m_state.commentDepth == 0) {
                    m_state.mode = Mode::Code;
                    break;
                }
            } else {
                ++i;
            }
        }
        i = std::min(i, m_n);
        if (i == stop && stop < m_n && m_state.mode == Mode::BlockComment) {
            m_state.mode = Mode::Code; // The body ends inside the comment.
            m_state.commentDepth = 0;
        }
        emit(TokenKind::Comment, start, i);
        return i;
    }

    // i is past the opening quote, or at the start of a continued piece.
    qsizetype quoted(qsizetype start, qsizetype i, char quote, bool escapes, TokenKind kind)
    {
        const qsizetype stop = limit(i);
        while (i < stop) {
            const char c = m_s[i];
            if (escapes && c == '\\') {
                i = std::min(i + 2, stop);
            } else if (c == quote) {
                if (peek(i + 1) == quote && i + 1 < stop) {
                    i += 2;
                } else {
                    ++i;
                    m_state.mode = Mode::Code;
                    emit(kind, start, i);
                    return i;
                }
            } else {
                ++i;
            }
        }
        if (stop < m_n) // The body ends inside the string.
            m_state.mode = Mode::Code;
        emit(kind, start, stop);
        return stop;
    }

    // $1, or a dollar quote, or a lone $.
    qsizetype dollar(qsizetype i, Expect expect)
    {
        if (isDigit(peek(i + 1))) {
            qsizetype end = i + 1;
            while (end < m_n && isDigit(m_s[end]))
                ++end;
            emit(TokenKind::Parameter, i, end);
            return end;
        }
        qsizetype end = i + 1;
        if (end < m_n && isIdentStart(m_s[end]) && m_s[end] != '$') {
            while (end < m_n && (isIdentStart(m_s[end]) || isDigit(m_s[end])))
                ++end;
        }
        if (peek(end) != '$') {
            emit(TokenKind::Unknown, i, i + 1);
            return i + 1;
        }
        ++end;
        const QByteArray tag = m_s.sliced(i, end - i).toByteArray();
        emit(TokenKind::DollarDelimiter, i, end);
        if (expect == Expect::Body && m_state.bodyTag.isEmpty()) {
            m_state.bodyTag = tag; // The body's contents are lexed as SQL.
        } else {
            m_state.mode = Mode::DollarString;
            m_state.tag = tag;
        }
        return end;
    }

    qsizetype dollarString(qsizetype i)
    {
        const qsizetype stop = limit(i);
        const qsizetype close = m_s.sliced(0, stop).indexOf(QByteArrayView(m_state.tag), i);
        if (close < 0) {
            if (stop < m_n) // The body ends inside the string.
                m_state.mode = Mode::Code;
            emit(TokenKind::DollarString, i, stop);
            return stop;
        }
        emit(TokenKind::DollarString, i, close);
        const qsizetype end = close + m_state.tag.size();
        emit(TokenKind::DollarDelimiter, close, end);
        m_state.mode = Mode::Code;
        m_state.tag.clear();
        return end;
    }

    qsizetype word(qsizetype i, Expect expect)
    {
        qsizetype end = i + 1;
        while (end < m_n && isIdentCont(m_s[end]))
            ++end;
        const QByteArrayView w = m_s.sliced(i, end - i);

        // String and identifier prefixes: E'', B'', X'', N'', U&'' and U&"".
        if (peek(end) == '\'' && w.size() == 1) {
            if (isWord(w, "e")) {
                m_state.mode = Mode::EscapeString;
                return quoted(i, end + 1, '\'', true, TokenKind::String);
            }
            if (isWord(w, "b") || isWord(w, "x") || isWord(w, "n")) {
                m_state.mode = Mode::String;
                return quoted(i, end + 1, '\'', false, TokenKind::String);
            }
        }
        if (isWord(w, "u") && peek(end) == '&' && (peek(end + 1) == '\'' || peek(end + 1) == '"')) {
            const bool identifier = peek(end + 1) == '"';
            m_state.mode = identifier ? Mode::QuotedIdentifier : Mode::String;
            return quoted(i, end + 2, identifier ? '"' : '\'', false,
                          identifier ? TokenKind::QuotedIdentifier : TokenKind::String);
        }

        emit(keywordCategory(w) ? TokenKind::Keyword : TokenKind::Identifier, i, end);
        if (isWord(w, "as") || isWord(w, "do"))
            m_state.expect = Expect::Body;
        else if (expect == Expect::Body && isWord(w, "language"))
            m_state.expect = Expect::LanguageThenBody; // DO LANGUAGE plpgsql $$ ... $$
        else if (expect == Expect::LanguageThenBody)
            m_state.expect = Expect::Body;
        return end;
    }

    qsizetype number(qsizetype i)
    {
        qsizetype end = i;
        if (m_s[i] == '0' && std::strchr("xXoObB", peek(i + 1)) && peek(i + 1) != '\0') {
            end = i + 2;
            while (end < m_n && (isHexDigit(m_s[end]) || m_s[end] == '_'))
                ++end;
        } else {
            while (end < m_n && (isDigit(m_s[end]) || m_s[end] == '_'))
                ++end;
            if (peek(end) == '.' && peek(end + 1) != '.') { // Not the start of 1..10.
                ++end;
                while (end < m_n && (isDigit(m_s[end]) || m_s[end] == '_'))
                    ++end;
            }
            if ((peek(end) == 'e' || peek(end) == 'E')
                && (isDigit(peek(end + 1))
                    || ((peek(end + 1) == '+' || peek(end + 1) == '-')
                        && isDigit(peek(end + 2))))) {
                end += 2;
                while (end < m_n && isDigit(m_s[end]))
                    ++end;
            }
        }
        emit(TokenKind::Number, i, end);
        return end;
    }

    // A run of operator characters; a comment start inside it ends it.
    qsizetype operatorRun(qsizetype i)
    {
        qsizetype end = i;
        while (end < m_n && isOperatorChar(m_s[end])) {
            if ((m_s[end] == '-' && peek(end + 1) == '-')
                || (m_s[end] == '/' && peek(end + 1) == '*'))
                break;
            ++end;
        }
        if (end == i) // Starts with a comment; code() handles those.
            end = i + 1;
        emit(TokenKind::Operator, i, end);
        return end;
    }

    // :: and := are operators; :name, :'name' and :"name" are psql variables.
    qsizetype colon(qsizetype i)
    {
        const char next = peek(i + 1);
        if (next == ':' || next == '=') {
            emit(TokenKind::Operator, i, i + 2);
            return i + 2;
        }
        if (isIdentStart(next)) {
            qsizetype end = i + 2;
            while (end < m_n && isIdentCont(m_s[end]))
                ++end;
            emit(TokenKind::PsqlVariable, i, end);
            return end;
        }
        if (next == '\'' || next == '"') {
            const qsizetype close = m_s.indexOf(next, i + 2);
            if (close > 0 && m_s.sliced(i, close - i).indexOf('\n') < 0) {
                emit(TokenKind::PsqlVariable, i, close + 1);
                return close + 1;
            }
        }
        emit(TokenKind::Punctuation, i, i + 1);
        return i + 1;
    }

    QByteArrayView m_s;
    qsizetype m_n;
    LexState &m_state;
    std::vector<Token> m_tokens;
};

} // namespace

std::vector<Token> tokenize(QByteArrayView text, LexState &state)
{
    return Lexer(text, state).run();
}

} // namespace slonisko::sql
