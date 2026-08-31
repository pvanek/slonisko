// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Internal to Slonisko::Catalog: what completion and semantic highlighting
// share about the names a statement refers to.

#include "sql/Lexer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <vector>

namespace slonisko::catalog::scope {

using sql::Token;
using sql::TokenKind;

// A name in scope that rows come from: a table, view, CTE, subquery or
// function in FROM.
struct Source
{
    QString name; // How it is referred to: its alias, else its name.
    QString schema;
    QString relation;
    QStringList columns; // Known columns of CTEs, subqueries and functions.
    bool derived = false;
    // Where the relation's name is in the statement, in bytes; -1 if unknown.
    qsizetype offset = -1;
    qsizetype length = 0;
};

struct Scope
{
    std::vector<Source> sources;
    std::vector<Source> ctes;
};

// Unquoted names fold to lowercase; quoted ones keep their case.
QString normalize(QByteArrayView text, TokenKind kind);
bool isName(TokenKind kind);
// A name as an identifier, double-quoted if it needs to be.
QString quoted(const QString &name);

// Significant tokens of a text, with helpers to read them.
class Tokens
{
public:
    explicit Tokens(QByteArrayView text) : m_text(text)
    {
        for (const Token &t : sql::tokenize(text)) {
            if (sql::isSignificant(t.kind))
                m_tokens.push_back(t);
        }
    }

    int size() const { return int(m_tokens.size()); }
    const Token &at(int i) const { return m_tokens[std::size_t(i)]; }
    QByteArrayView text(int i) const { return m_text.sliced(at(i).offset, at(i).length); }
    QString name(int i) const { return normalize(text(i), at(i).kind); }

    bool is(int i, const char *word) const
    {
        return i >= 0 && i < size()
            && (at(i).kind == TokenKind::Keyword || at(i).kind == TokenKind::Identifier)
            && text(i).compare(QByteArrayView(word), Qt::CaseInsensitive) == 0;
    }
    bool isPunct(int i, char c) const
    {
        return i >= 0 && i < size() && at(i).kind == TokenKind::Punctuation
            && text(i) == QByteArrayView(&c, 1);
    }
    bool isAnyOf(int i, std::initializer_list<const char *> words) const
    {
        return std::ranges::any_of(words, [&](const char *w) { return is(i, w); });
    }
    // Index past the parenthesis matching the one at i.
    int skipParens(int i) const
    {
        int depth = 0;
        for (; i < size(); ++i) {
            if (isPunct(i, '('))
                ++depth;
            else if (isPunct(i, ')') && --depth == 0)
                return i + 1;
        }
        return size();
    }

private:
    QByteArrayView m_text;
    std::vector<Token> m_tokens;
};

// Keywords that end a FROM item rather than name its alias.
bool endsFromItem(const Tokens &t, int i);
bool canBeAlias(const Tokens &t, int i);
// Scope from tokens alone, for statements that do not parse.
Scope tokenScope(const Tokens &t);

// Helpers for pg_query's JSON parse trees.
QStringList stringList(const QJsonArray &items);
// Output column names of a query: its targets' aliases or column names.
QStringList targetNames(const QJsonObject &selectStmt);
Source rangeVarSource(const QJsonObject &rangeVar);
// A RangeVar field: wrapped as {"RangeVar": {...}} where the field is a
// generic node, bare where it is typed, like InsertStmt.relation.
QJsonObject unwrap(const QJsonObject &value, const char *node);
// pg_query_parse() as JSON, or nullopt if the text does not parse.
std::optional<QJsonObject> parse(const QByteArray &sql);

} // namespace slonisko::catalog::scope
