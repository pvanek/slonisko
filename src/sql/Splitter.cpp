// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Splitter.h"

#include <QByteArrayView>

#include <algorithm>
#include <array>

namespace slonisko::sql {

namespace {

using Kind = StatementSpan::Kind;

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isLineEnd(char c)
{
    return c == '\n' || c == '\r';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Bytes >= 0x80 are parts of UTF-8 sequences, which PostgreSQL allows in
// identifiers and dollar-quote tags.
bool isIdentStart(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}

bool isIdentCont(char c)
{
    return isIdentStart(c) || isDigit(c) || c == '$';
}

bool isWord(QByteArrayView word, QByteArrayView keyword)
{
    return word.compare(keyword, Qt::CaseInsensitive) == 0;
}

// Meta-commands that send the query buffer to the server, ending the statement.
bool executesQueryBuffer(QByteArrayView name)
{
    static constexpr std::array names {"g",     "gx",    "gdesc",        "gexec",       "gset",
                                       "watch", "parse", "crosstabview", "sendpipeline"};
    return std::ranges::any_of(names, [&](const char *n) { return name == n; });
}

// Meta-commands whose argument is the rest of the line, backslashes included.
bool takesWholeLine(QByteArrayView name)
{
    static constexpr std::array names {"copy", "ef", "ev",   "sf",         "sf+", "sv",
                                       "sv+",  "h",  "help", "unrestrict", "!",   "?"};
    return std::ranges::any_of(names, [&](const char *n) { return name == n; });
}

// Whether \copy arguments read the data inline, i.e. contain "from stdin".
bool copyArgsReadStdin(QByteArrayView args)
{
    QByteArrayView previous;
    qsizetype i = 0;
    while (i < args.size()) {
        const char c = args[i];
        if (isIdentStart(c)) {
            qsizetype end = i + 1;
            while (end < args.size() && isIdentCont(args[end]))
                ++end;
            const QByteArrayView word = args.sliced(i, end - i);
            if (isWord(previous, "from") && isWord(word, "stdin"))
                return true;
            previous = word;
            i = end;
        } else if (c == '\'' || c == '"') {
            const qsizetype close = args.indexOf(c, i + 1);
            i = close < 0 ? args.size() : close + 1;
            previous = {};
        } else {
            if (!isSpace(c))
                previous = {};
            ++i;
        }
    }
    return false;
}

class Splitter
{
public:
    explicit Splitter(const QByteArray &script) : m_s(script), m_n(script.size()) { }

    std::vector<StatementSpan> run()
    {
        qsizetype i = 0;
        while (i < m_n) {
            const char c = m_s[i];
            if (isSpace(c)) {
                ++i;
            } else if (c == '-' && peek(i + 1) == '-') {
                i = lineEnd(i + 2);
            } else if (c == '/' && peek(i + 1) == '*') {
                i = skipBlockComment(i + 2);
            } else if (c == ';' && m_parenDepth == 0 && m_blockDepth == 0) {
                i = finish(true) ? copyData(i + 1) : i + 1;
            } else if (c == '\\' && peek(i + 1) == ';') {
                addToStatement(i, i + 2);
                m_prevWord = {};
                i += 2;
            } else if (c == '\\' && peek(i + 1) == '\\') {
                i += 2; // A separator with no command before it.
            } else if (c == '\\') {
                i = metaCommand(i);
            } else {
                const qsizetype end = token(i);
                addToStatement(i, end);
                i = end;
            }
        }
        finish(false);
        return std::move(m_out);
    }

private:
    char peek(qsizetype i) const { return i < m_n ? m_s[i] : '\0'; }

    QByteArrayView view(qsizetype from, qsizetype to) const
    {
        return QByteArrayView(m_s).sliced(from, to - from);
    }

    void addToStatement(qsizetype from, qsizetype to)
    {
        if (m_start < 0)
            m_start = from;
        m_end = to;
    }

    // Consumes the SQL token at i, updates the nesting state and returns its end.
    qsizetype token(qsizetype i)
    {
        const char c = m_s[i];
        QByteArrayView word;
        qsizetype end = i + 1;

        if (c == '\'') {
            end = skipQuoted(i + 1, '\'', false);
        } else if (c == '"') {
            end = skipQuoted(i + 1, '"', false);
        } else if (c == '$' && dollarTagEnd(i) > 0) {
            end = skipDollarQuoted(i, dollarTagEnd(i));
        } else if (isIdentStart(c)) {
            while (end < m_n && isIdentCont(m_s[end]))
                ++end;
            word = view(i, end);
            if (peek(end) == '\'' && isWord(word, "e")) {
                end = skipQuoted(end + 1, '\'', true);
                word = {};
            } else {
                trackWord(word);
            }
        } else if (isDigit(c)) {
            // Numbers like 1.5e10, 0x1F or 1_000; only need to keep "e" from
            // being read as an escape-string prefix.
            while (end < m_n && (isIdentCont(m_s[end]) || m_s[end] == '.'))
                ++end;
        } else if (c == '(') {
            ++m_parenDepth;
        } else if (c == ')') {
            // Tolerate stray parentheses while text is being edited.
            if (m_parenDepth > 0)
                --m_parenDepth;
        }

        m_prevWord = word;
        return end;
    }

    // Tracks BEGIN ATOMIC ... END bodies of SQL-standard functions and
    // procedures, CASE ... END inside them, and COPY ... FROM STDIN.
    void trackWord(QByteArrayView word)
    {
        if (m_parenDepth > 0)
            return;
        if (m_start < 0)
            m_isCopy = isWord(word, "copy");
        else if (m_isCopy && isWord(m_prevWord, "from") && isWord(word, "stdin"))
            m_copyFromStdin = true;

        if (isWord(m_prevWord, "begin") && isWord(word, "atomic")) {
            ++m_blockDepth;
        } else if (m_blockDepth > 0 && isWord(word, "case")) {
            ++m_blockDepth;
        } else if (m_blockDepth > 0 && isWord(word, "end")) {
            --m_blockDepth;
        }
    }

    // A psql meta-command at the backslash at i. Returns where to continue.
    qsizetype metaCommand(qsizetype i)
    {
        qsizetype j = i + 1;
        while (j < m_n && !isSpace(m_s[j]) && m_s[j] != '\\')
            ++j;
        const QByteArrayView name = view(i + 1, j);

        qsizetype end = j; // End of the command text.
        qsizetype next = j; // Where SQL scanning resumes.
        if (takesWholeLine(name)) {
            next = lineEnd(j);
            end = next;
            while (end > j && isSpace(m_s[end - 1]))
                --end;
        } else {
            // Arguments end at the line end, or at an unquoted backslash,
            // which starts another command; \\ just ends the arguments.
            while (next < m_n && !isLineEnd(m_s[next])) {
                const char c = m_s[next];
                if (c == '\\') {
                    if (peek(next + 1) == '\\')
                        next += 2;
                    break;
                }
                if (c == '\'' || c == '"' || c == '`')
                    next = skipArgQuoted(next + 1, c);
                else
                    ++next;
                if (!isSpace(c))
                    end = next;
            }
        }

        // Unlike psql, which keeps the query buffer across other commands, a
        // meta-command always ends the statement before it.
        bool readsData = m_start >= 0 && finish(executesQueryBuffer(name));
        m_out.push_back({i, end - i, Kind::PsqlCommand, true});
        if (isWord(name, "copy") && copyArgsReadStdin(view(j, end)))
            readsData = true;
        return readsData ? copyData(next) : next;
    }

    // Inline COPY data starts on the line after the command (the rest of the
    // command's line is ignored) and ends at a line consisting of \. alone.
    qsizetype copyData(qsizetype i)
    {
        i = nextLine(i);
        const qsizetype start = i;
        while (i < m_n) {
            if (m_s[i] == '\\' && peek(i + 1) == '.' && (i + 2 == m_n || isLineEnd(m_s[i + 2]))) {
                m_out.push_back({start, i - start, Kind::CopyData, true});
                return nextLine(i);
            }
            i = nextLine(i);
        }
        m_out.push_back({start, m_n - start, Kind::CopyData, false});
        return m_n;
    }

    qsizetype lineEnd(qsizetype i) const
    {
        while (i < m_n && !isLineEnd(m_s[i]))
            ++i;
        return i;
    }

    // The start of the line after the one containing i.
    qsizetype nextLine(qsizetype i) const
    {
        i = lineEnd(i);
        if (peek(i) == '\r')
            ++i;
        if (peek(i) == '\n')
            ++i;
        return i;
    }

    // Block comments nest in PostgreSQL.
    qsizetype skipBlockComment(qsizetype i) const
    {
        int depth = 1;
        while (i < m_n) {
            if (m_s[i] == '/' && peek(i + 1) == '*') {
                ++depth;
                i += 2;
            } else if (m_s[i] == '*' && peek(i + 1) == '/') {
                i += 2;
                if (--depth == 0)
                    return i;
            } else {
                ++i;
            }
        }
        return m_n;
    }

    // i is just past the opening quote. A doubled quote is an escaped quote.
    qsizetype skipQuoted(qsizetype i, char quote, bool backslashEscapes) const
    {
        while (i < m_n) {
            const char c = m_s[i];
            if (backslashEscapes && c == '\\') {
                i += 2;
            } else if (c == quote) {
                if (peek(i + 1) != quote)
                    return i + 1;
                i += 2;
            } else {
                ++i;
            }
        }
        return m_n;
    }

    // A quoted meta-command argument; it cannot span lines. Single-quoted
    // arguments also take backslash escapes.
    qsizetype skipArgQuoted(qsizetype i, char quote) const
    {
        while (i < m_n && !isLineEnd(m_s[i])) {
            const char c = m_s[i];
            if (quote == '\'' && c == '\\' && i + 1 < m_n && !isLineEnd(m_s[i + 1])) {
                i += 2;
            } else if (c == quote) {
                if (peek(i + 1) != quote)
                    return i + 1;
                i += 2;
            } else {
                ++i;
            }
        }
        return i;
    }

    // Returns the end of a $tag$ opening delimiter at i, or -1 if there is
    // none there ($1 is a parameter, not a dollar quote).
    qsizetype dollarTagEnd(qsizetype i) const
    {
        qsizetype j = i + 1;
        if (j < m_n && isIdentStart(m_s[j])) {
            while (j < m_n && (isIdentStart(m_s[j]) || isDigit(m_s[j])))
                ++j;
        }
        return peek(j) == '$' ? j + 1 : -1;
    }

    qsizetype skipDollarQuoted(qsizetype i, qsizetype tagEnd) const
    {
        const QByteArrayView delimiter = view(i, tagEnd);
        const qsizetype close = QByteArrayView(m_s).indexOf(delimiter, tagEnd);
        return close < 0 ? m_n : close + delimiter.size();
    }

    // Ends the current statement, if any. Returns true when inline COPY data
    // follows it.
    bool finish(bool terminated)
    {
        const bool readsData = m_start >= 0 && terminated && m_copyFromStdin;
        if (m_start >= 0)
            m_out.push_back({m_start, m_end - m_start, Kind::Sql, terminated});
        m_start = -1;
        m_parenDepth = 0;
        m_blockDepth = 0;
        m_prevWord = {};
        m_isCopy = false;
        m_copyFromStdin = false;
        return readsData;
    }

    const QByteArray &m_s;
    const qsizetype m_n;
    std::vector<StatementSpan> m_out;

    qsizetype m_start = -1; // First token of the current statement, -1 if none yet.
    qsizetype m_end = 0; // End of its last token.
    int m_parenDepth = 0;
    int m_blockDepth = 0; // BEGIN ATOMIC and CASE nesting.
    QByteArrayView m_prevWord; // The previous token, if it was a word.
    bool m_isCopy = false; // The statement starts with COPY.
    bool m_copyFromStdin = false;
};

} // namespace

std::vector<StatementSpan> splitStatements(const QByteArray &utf8Script)
{
    return Splitter(utf8Script).run();
}

int statementAt(const QByteArray &utf8Script, const std::vector<StatementSpan> &spans,
                qsizetype pos)
{
    const QByteArrayView text(utf8Script);
    auto sameLine = [&](qsizetype a, qsizetype b) {
        const qsizetype from = std::min(a, b);
        const qsizetype to = std::min(std::max(a, b), text.size());
        return text.sliced(from, to - from).indexOf('\n') < 0;
    };
    auto owner = [&](int i) {
        // Inline data belongs to the COPY before it.
        return spans[std::size_t(i)].kind == StatementSpan::Kind::CopyData && i > 0 ? i - 1 : i;
    };

    int before = -1;
    for (int i = 0; i < int(spans.size()); ++i) {
        const StatementSpan &s = spans[std::size_t(i)];
        qsizetype end = s.offset + s.length;
        if (s.kind == StatementSpan::Kind::Sql && s.terminated) // Include the semicolon.
            end = std::min(text.size(), text.indexOf(';', end) + 1);
        if (pos >= s.offset && pos <= end)
            return owner(i);
        if (end <= pos)
            before = i;
    }
    if (before >= 0) {
        const StatementSpan &s = spans[std::size_t(before)];
        if (sameLine(s.offset + s.length, pos))
            return owner(before);
    }
    const int after = before + 1;
    if (after < int(spans.size()) && sameLine(pos, spans[std::size_t(after)].offset))
        return owner(after);
    return -1;
}

} // namespace slonisko::sql
