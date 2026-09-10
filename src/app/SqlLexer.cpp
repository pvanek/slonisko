// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SqlLexer.h"

#include "sql/Keywords.h"

#include "Qsci.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QMutex>
#include <QPalette>

namespace slonisko {

using sql::TokenKind;

namespace {

// Dollar-quote tags are interned: a line state holds a small id, not the tag.
// Keyed by the tag, not the line, so inserting lines does not invalidate it.
class TagTable
{
public:
    int id(const QByteArray &tag)
    {
        if (tag.isEmpty())
            return 0;
        QMutexLocker lock(&m_mutex);
        const int found = int(m_tags.indexOf(tag));
        if (found >= 0)
            return found + 1;
        if (m_tags.size() >= MaxTags)
            return 0; // Out of ids; such a line restyles as if unquoted.
        m_tags.append(tag);
        return int(m_tags.size());
    }

    QByteArray tag(int id)
    {
        QMutexLocker lock(&m_mutex);
        return id > 0 && id <= m_tags.size() ? m_tags[id - 1] : QByteArray();
    }

    static constexpr int MaxTags = (1 << 11) - 1;

private:
    QMutex m_mutex;
    QByteArrayList m_tags;
};

TagTable &tags()
{
    static TagTable table;
    return table;
}

// Bit layout of a line state: mode (3), comment depth (4), tag id (11), body
// tag id (11), expect (2). Stays below 2^31.
constexpr int ModeBits = 3, DepthBits = 4, TagBits = 11, ExpectBits = 2;

} // namespace

int SqlLexer::encode(const sql::LexState &s)
{
    int v = int(s.mode);
    v |= std::min(s.commentDepth, (1 << DepthBits) - 1) << ModeBits;
    v |= tags().id(s.tag) << (ModeBits + DepthBits);
    v |= tags().id(s.bodyTag) << (ModeBits + DepthBits + TagBits);
    v |= int(s.expect) << (ModeBits + DepthBits + 2 * TagBits);
    return v;
}

sql::LexState SqlLexer::decode(int v)
{
    auto bits = [&](int shift, int count) { return (v >> shift) & ((1 << count) - 1); };
    sql::LexState s;
    s.mode = sql::LexState::Mode(bits(0, ModeBits));
    s.commentDepth = bits(ModeBits, DepthBits);
    s.tag = tags().tag(bits(ModeBits + DepthBits, TagBits));
    s.bodyTag = tags().tag(bits(ModeBits + DepthBits + TagBits, TagBits));
    s.expect = sql::LexState::Expect(bits(ModeBits + DepthBits + 2 * TagBits, ExpectBits));
    return s;
}

SqlLexer::SqlLexer(QObject *parent) : QsciLexerCustom(parent)
{
    m_dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
}

void SqlLexer::refreshPalette()
{
    m_dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
}

QString SqlLexer::description(int style) const
{
    switch (style) {
    case Default:
        return tr("Default");
    case Comment:
        return tr("Comment");
    case Keyword:
        return tr("Keyword");
    case UnreservedKeyword:
        return tr("Unreserved keyword");
    case Type:
        return tr("Type");
    case Identifier:
        return tr("Identifier");
    case QuotedIdentifier:
        return tr("Quoted identifier");
    case String:
        return tr("String");
    case DollarDelimiter:
        return tr("Dollar quote");
    case DollarString:
        return tr("Dollar-quoted string");
    case Number:
        return tr("Number");
    case Operator:
        return tr("Operator");
    case Punctuation:
        return tr("Punctuation");
    case Parameter:
        return tr("Parameter");
    case PsqlVariable:
        return tr("psql variable");
    case PsqlCommand:
        return tr("psql command");
    case Unknown:
        return tr("Unknown");
    default:
        return {};
    }
}

SqlLexer::Style SqlLexer::styleFor(const sql::Token &t, QByteArrayView text)
{
    switch (t.kind) {
    case TokenKind::Whitespace:
        return Default;
    case TokenKind::Comment:
        return Comment;
    case TokenKind::Keyword: {
        const QByteArrayView word = text.sliced(t.offset, t.length);
        if (sql::isBuiltinTypeName(word))
            return Type;
        const auto category = sql::keywordCategory(word);
        return category == sql::KeywordCategory::Unreserved ? UnreservedKeyword : Keyword;
    }
    case TokenKind::Identifier:
        return sql::isBuiltinTypeName(text.sliced(t.offset, t.length)) ? Type : Identifier;
    case TokenKind::QuotedIdentifier:
        return QuotedIdentifier;
    case TokenKind::String:
        return String;
    case TokenKind::DollarDelimiter:
        return DollarDelimiter;
    case TokenKind::DollarString:
        return DollarString;
    case TokenKind::Number:
        return Number;
    case TokenKind::Operator:
        return Operator;
    case TokenKind::Punctuation:
        return Punctuation;
    case TokenKind::Parameter:
        return Parameter;
    case TokenKind::PsqlVariable:
        return PsqlVariable;
    case TokenKind::PsqlCommand:
        return PsqlCommand;
    case TokenKind::Unknown:
        return Unknown;
    }
    return Default;
}

int SqlLexer::stateDepth(const sql::LexState &s)
{
    using Mode = sql::LexState::Mode;
    return (s.mode != Mode::Code ? 1 : 0) + (s.bodyTag.isEmpty() ? 0 : 1);
}

int SqlLexer::foldDepthAfter(const std::vector<sql::Token> &tokens, QByteArrayView text, int depth,
                             const sql::LexState &start)
{
    QByteArray openBody = start.bodyTag; // The delimiter that ends the body we are in.
    std::vector<const sql::Token *> significant;
    for (const sql::Token &t : tokens) {
        if (sql::isSignificant(t.kind))
            significant.push_back(&t);
    }
    auto word = [&](std::size_t i) -> QByteArray {
        if (i >= significant.size())
            return {};
        const sql::Token &t = *significant[i];
        if (t.kind != TokenKind::Keyword && t.kind != TokenKind::Identifier)
            return {};
        return text.sliced(t.offset, t.length).toByteArray().toLower();
    };

    for (std::size_t i = 0; i < significant.size(); ++i) {
        const sql::Token &t = *significant[i];
        if (t.kind == TokenKind::PsqlCommand)
            continue; // Not part of any statement.
        if (depth == 0)
            depth = 1; // A statement starts.
        // A body's end: back to its statement's level, whatever the body
        // held. Its language may not be PL/pgSQL, whose IF and LOOP this
        // counts; Python's "if" has no END IF.
        if (t.kind == TokenKind::DollarDelimiter && !t.inBody) {
            const QByteArrayView tag = text.sliced(t.offset, t.length);
            if (!openBody.isEmpty() && tag == openBody) {
                openBody.clear();
                depth = 1;
            } else if (i + 1 < significant.size() && significant[i + 1]->inBody) {
                openBody = tag.toByteArray();
            }
            continue;
        }
        if (t.kind == TokenKind::Punctuation) {
            const char c = text[t.offset];
            if (c == '(')
                ++depth;
            else if (c == ')' && depth > 1)
                --depth;
            else if (c == ';' && depth == 1 && !t.inBody)
                depth = 0; // The statement ends.
            continue;
        }
        const QByteArray w = word(i);
        if (w.isEmpty())
            continue;
        const QByteArray before = i > 0 ? word(i - 1) : QByteArray();
        if (w == "case") {
            if (before != "end")
                ++depth;
        } else if (w == "end") {
            if (depth > 1)
                --depth;
        } else if (w == "begin") {
            if (t.inBody || word(i + 1) == "atomic")
                ++depth;
        } else if (t.inBody && (w == "if" || w == "loop")) {
            // END IF and END LOOP close, IF EXISTS in DDL opens nothing.
            if (before != "end" && word(i + 1) != "not" && word(i + 1) != "exists")
                ++depth;
        }
    }
    return depth;
}

void SqlLexer::styleText(int start, int end)
{
    QsciScintilla *e = editor();
    if (!e)
        return;
    const long length = e->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    int line = int(e->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, start));
    const int lastLine = int(e->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, end));
    const int lineCount = e->lines();

    // A pointer result: SendScintilla() returns long, too small on 64-bit Windows.
    const auto *document = static_cast<const char *>(
        e->SendScintillaPtrResult(QsciScintillaBase::SCI_GETCHARACTERPOINTER));
    sql::LexState state = line > 0
        ? decode(int(e->SendScintilla(QsciScintillaBase::SCI_GETLINESTATE, line - 1)))
        : sql::LexState();

    // The fold level of a line is its depth at the start; the structural
    // part of it carries on from line to line.
    constexpr int Base = QsciScintillaBase::SC_FOLDLEVELBASE;
    constexpr int NumberMask = QsciScintillaBase::SC_FOLDLEVELNUMBERMASK;
    constexpr int Header = QsciScintillaBase::SC_FOLDLEVELHEADERFLAG;
    auto level
        = [e](int l) { return int(e->SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL, l)); };
    int structure
        = line > 0 ? std::max(0, (level(line) & NumberMask) - Base - stateDepth(state)) : 0;

    for (; line < lineCount; ++line) {
        const long from = e->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line);
        const long to = line + 1 < lineCount
            ? e->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line + 1)
            : length;
        const QByteArrayView text(document + from, to - from);

        startStyling(int(from));
        const int startDepth = structure + stateDepth(state);
        const sql::LexState startState = state;
        const std::vector<sql::Token> tokens = sql::tokenize(text, state);
        bool afterDot = false; // In o.name, name is a column, not a keyword or type.
        for (const sql::Token &t : tokens) {
            Style style = styleFor(t, text);
            if (afterDot && (style == Keyword || style == UnreservedKeyword || style == Type))
                style = Identifier;
            if (sql::isSignificant(t.kind))
                afterDot = t.kind == TokenKind::Punctuation && text[t.offset] == '.';
            setStyling(int(t.length), style);
        }

        structure = foldDepthAfter(tokens, text, structure, startState);
        const int endDepth = structure + stateDepth(state);
        e->SendScintilla(QsciScintillaBase::SCI_SETFOLDLEVEL, line,
                         (Base + startDepth) | (endDepth > startDepth ? Header : 0));
        // Where the next line starts; it sets its own header flag when styled.
        int nextLevel = Base + endDepth;
        if (line + 1 < lineCount) {
            nextLevel |= level(line + 1) & Header;
            const int previousNext = level(line + 1);
            e->SendScintilla(QsciScintillaBase::SCI_SETFOLDLEVEL, line + 1, nextLevel);
            nextLevel = previousNext == nextLevel ? nextLevel : -1;
        }

        const int encoded = encode(state);
        const int previous = int(e->SendScintilla(QsciScintillaBase::SCI_GETLINESTATE, line));
        e->SendScintilla(QsciScintillaBase::SCI_SETLINESTATE, line, encoded);
        // Past the requested lines, go on only while this edit changes what
        // the following lines start with: lexer state or fold depth.
        if (line >= lastLine && encoded == previous && nextLevel >= 0)
            break;
    }
}

QColor SqlLexer::defaultColor(int style) const
{
    return color(style, m_dark);
}

QColor SqlLexer::color(int style, bool isDark)
{
    // Light and dark variants of a restrained palette.
    auto pick = [isDark](const char *light, const char *dark) {
        return QColor::fromString(QLatin1String(isDark ? dark : light));
    };
    switch (style) {
    case Comment:
        return pick("#6a737d", "#8b949e");
    case Keyword:
        return pick("#0033b3", "#79b8ff");
    case UnreservedKeyword:
        return pick("#3a5cb3", "#9ecbff");
    case Type:
        return pick("#7a3e9d", "#d2a8ff");
    case QuotedIdentifier:
        return pick("#005c5c", "#56d4dd");
    case String:
    case DollarString:
        return pick("#067d17", "#7ee787");
    case DollarDelimiter:
        return pick("#8a6d00", "#e3b341");
    case Number:
        return pick("#1750eb", "#79c0ff");
    case Operator:
        return pick("#871094", "#ff7b72");
    case Parameter:
    case PsqlVariable:
        return pick("#b35900", "#ffa657");
    case PsqlCommand:
        return pick("#8a6d00", "#e3b341");
    case Unknown:
        return pick("#c62828", "#ff6b6b");
    default:
        return QGuiApplication::palette().color(QPalette::Text);
    }
}

QColor SqlLexer::defaultPaper(int) const
{
    return QGuiApplication::palette().color(QPalette::Base);
}

QFont SqlLexer::defaultFont(int style) const
{
    // No bold keywords: semantic highlighting recolors text but cannot
    // change its font, and a bold SQL keyword in a Python body would stand out.
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (style == Comment)
        font.setItalic(true);
    return font;
}

const char *SqlLexer::wordCharacters() const
{
    return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$";
}

} // namespace slonisko
