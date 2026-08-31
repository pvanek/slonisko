// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sql/Lexer.h"

#include "Qsci.h"

namespace slonisko {

// Syntax highlighting for PostgreSQL SQL: styles QScintilla text with
// sql::tokenize(), line by line. Each line's end state is kept in Scintilla's
// per-line state, so an edit restyles from its line on, and only as far as
// the state it leaves behind differs from before.
class SqlLexer : public QsciLexerCustom
{
    Q_OBJECT

public:
    enum Style {
        Default,
        Comment,
        Keyword,
        UnreservedKeyword, // Also usable as a name, like "name" or "type".
        Type,
        Identifier,
        QuotedIdentifier,
        String,
        DollarDelimiter,
        DollarString,
        Number,
        Operator,
        Punctuation,
        Parameter,
        PsqlVariable,
        PsqlCommand,
        Unknown,
        StyleCount,
    };

    explicit SqlLexer(QObject *parent = nullptr);

    const char *language() const override { return "PostgreSQL"; }
    QString description(int style) const override;
    void styleText(int start, int end) override;

    QColor defaultColor(int style) const override;
    QColor defaultPaper(int style) const override;
    QFont defaultFont(int style) const override;
    const char *wordCharacters() const override;

    static Style styleFor(const sql::Token &token, QByteArrayView text);

    // Packing a lexer state into Scintilla's per-line int, for tests.
    static int encode(const sql::LexState &state);
    static sql::LexState decode(int value);

private:
    bool m_dark = false;
};

} // namespace slonisko
