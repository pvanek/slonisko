// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/EmbeddedLexer.h"

#include <QSet>

#include <cstring>

namespace slonisko::sql {

namespace {

using Kind = EmbeddedToken::Kind;
using L = EmbeddedLanguage;

QSet<QByteArray> words(std::initializer_list<const char *> list)
{
    QSet<QByteArray> out;
    for (const char *w : list)
        out.insert(w);
    return out;
}

const QSet<QByteArray> &keywordsOf(L language)
{
    static const QSet<QByteArray> python = words(
        {"False", "None",     "True",  "and",    "as",   "assert", "async",  "await",    "break",
         "class", "continue", "def",   "del",    "elif", "else",   "except", "finally",  "for",
         "from",  "global",   "if",    "import", "in",   "is",     "lambda", "nonlocal", "not",
         "or",    "pass",     "raise", "return", "try",  "while",  "with",   "yield"});
    static const QSet<QByteArray> perl
        = words({"my",    "our", "local",   "sub",     "if",    "elsif", "else", "unless", "while",
                 "until", "for", "foreach", "return",  "last",  "next",  "redo", "die",    "warn",
                 "print", "use", "package", "require", "ne",    "eq",    "lt",   "gt",     "le",
                 "ge",    "and", "or",      "not",     "undef", "shift"});
    static const QSet<QByteArray> tcl
        = words({"proc",    "set",    "if",     "else",  "elseif", "while",    "for",
                 "foreach", "return", "expr",   "puts",  "break",  "continue", "switch",
                 "catch",   "error",  "global", "upvar", "incr",   "list"});
    static const QSet<QByteArray> javascript
        = words({"var",      "let",        "const", "function", "return",    "if",      "else",
                 "for",      "while",      "do",    "switch",   "case",      "default", "break",
                 "continue", "new",        "this",  "null",     "undefined", "true",    "false",
                 "typeof",   "instanceof", "try",   "catch",    "finally",   "throw",   "class",
                 "of",       "in",         "yield", "async",    "await",     "delete",  "void"});
    static const QSet<QByteArray> lua
        = words({"and",      "break",  "do",   "else", "elseif", "end",  "false", "for",
                 "function", "goto",   "if",   "in",   "local",  "nil",  "not",   "or",
                 "repeat",   "return", "then", "true", "until",  "while"});
    static const QSet<QByteArray> r
        = words({"if", "else", "repeat", "while", "function", "for", "in", "next", "break", "TRUE",
                 "FALSE", "NULL", "Inf", "NaN", "NA", "return"});
    static const QSet<QByteArray> shell
        = words({"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case",
                 "esac", "in", "function", "return", "local", "export", "echo"});
    static const QSet<QByteArray> none;
    switch (language) {
    case L::Python:
        return python;
    case L::Perl:
        return perl;
    case L::Tcl:
        return tcl;
    case L::JavaScript:
        return javascript;
    case L::Lua:
        return lua;
    case L::R:
        return r;
    case L::Shell:
        return shell;
    default:
        return none;
    }
}

bool isIdent(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
        || static_cast<unsigned char>(c) >= 0x80;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

class Lexer
{
public:
    Lexer(QByteArrayView s, L language) : m_s(s), m_n(s.size()), m_language(language) { }

    std::vector<EmbeddedToken> run()
    {
        const QSet<QByteArray> &keywords = keywordsOf(m_language);
        qsizetype i = 0;
        while (i < m_n) {
            const char c = m_s[i];
            const qsizetype comment = blockComment(i);
            const qsizetype text = comment > i ? i : string(i);
            if (lineComment(i)) {
                const qsizetype end = lineEnd(i);
                emit(Kind::Comment, i, end);
                i = end;
            } else if (comment > i) {
                emit(Kind::Comment, i, comment);
                i = comment;
            } else if (text > i) {
                emit(Kind::String, i, text);
                i = text;
            } else if (isDigit(c) && (i == 0 || !isIdent(m_s[i - 1]))) {
                qsizetype end = i + 1;
                while (end < m_n && (isIdent(m_s[end]) || m_s[end] == '.'))
                    ++end;
                emit(Kind::Number, i, end);
                i = end;
            } else if (isIdent(c)) {
                qsizetype end = i + 1;
                while (end < m_n && isIdent(m_s[end]))
                    ++end;
                if (keywords.contains(m_s.sliced(i, end - i).toByteArray()))
                    emit(Kind::Keyword, i, end);
                i = end;
            } else {
                ++i;
            }
        }
        return std::move(m_tokens);
    }

private:
    char peek(qsizetype i) const { return i < m_n ? m_s[i] : '\0'; }
    bool startsWith(qsizetype i, const char *text) const
    {
        return m_s.sliced(i).startsWith(QByteArrayView(text));
    }
    void emit(Kind kind, qsizetype from, qsizetype to)
    {
        m_tokens.push_back({kind, from, to - from});
    }

    qsizetype lineEnd(qsizetype i) const
    {
        while (i < m_n && m_s[i] != '\n')
            ++i;
        return i;
    }

    bool lineComment(qsizetype i) const
    {
        switch (m_language) {
        case L::Python:
        case L::Perl:
        case L::R:
        case L::Shell:
        case L::Tcl:
            return m_s[i] == '#';
        case L::JavaScript:
            return startsWith(i, "//");
        case L::Lua:
            return startsWith(i, "--") && !startsWith(i, "--[[");
        default:
            return false;
        }
    }

    // The end of a block comment starting at i, or i if there is none.
    qsizetype blockComment(qsizetype i) const
    {
        auto until = [&](qsizetype from, const char *close) {
            const qsizetype at = m_s.indexOf(QByteArrayView(close), from);
            return at < 0 ? m_n : at + qsizetype(std::strlen(close));
        };
        if (m_language == L::JavaScript && startsWith(i, "/*"))
            return until(i + 2, "*/");
        if (m_language == L::Lua && startsWith(i, "--[["))
            return until(i + 4, "]]");
        return i;
    }

    // The end of a string starting at i, or i if there is none.
    qsizetype string(qsizetype i) const
    {
        const char c = m_s[i];
        if (m_language == L::Python && (startsWith(i, "'''") || startsWith(i, "\"\"\""))) {
            const qsizetype close = m_s.indexOf(m_s.sliced(i, 3), i + 3);
            return close < 0 ? m_n : close + 3;
        }
        if (m_language == L::Lua && startsWith(i, "[["))
            return [&] {
                const qsizetype close = m_s.indexOf(QByteArrayView("]]"), i + 2);
                return close < 0 ? m_n : close + 2;
            }();
        const bool quote = c == '\'' || c == '"' || (c == '`' && m_language == L::JavaScript);
        if (!quote || m_language == L::Other)
            return i;
        qsizetype j = i + 1;
        while (j < m_n) {
            if (m_s[j] == '\\') {
                j += 2;
            } else if (m_s[j] == c) {
                return j + 1;
            } else if (m_s[j] == '\n' && c != '`' && m_language != L::Tcl
                       && m_language != L::Perl) {
                return j; // Unterminated on this line.
            } else {
                ++j;
            }
        }
        return m_n;
    }

    QByteArrayView m_s;
    qsizetype m_n;
    L m_language;
    std::vector<EmbeddedToken> m_tokens;
};

} // namespace

EmbeddedLanguage embeddedLanguage(const QString &pgLanguage)
{
    const QString l = pgLanguage.toLower();
    if (l == QLatin1String("sql") || l == QLatin1String("plpgsql"))
        return L::Sql;
    if (l.startsWith(QLatin1String("plpython")))
        return L::Python;
    if (l.startsWith(QLatin1String("plperl")))
        return L::Perl;
    if (l.startsWith(QLatin1String("pltcl")))
        return L::Tcl;
    if (l == QLatin1String("plv8") || l == QLatin1String("plls") || l == QLatin1String("pljs"))
        return L::JavaScript;
    if (l.startsWith(QLatin1String("pllua")))
        return L::Lua;
    if (l == QLatin1String("plr"))
        return L::R;
    if (l == QLatin1String("plsh"))
        return L::Shell;
    return L::Other;
}

std::vector<EmbeddedToken> tokenizeEmbedded(QByteArrayView body, EmbeddedLanguage language)
{
    if (language == L::Sql || language == L::Other)
        return {};
    return Lexer(body, language).run();
}

} // namespace slonisko::sql
