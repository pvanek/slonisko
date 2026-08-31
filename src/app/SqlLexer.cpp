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

    for (; line < lineCount; ++line) {
        const long from = e->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line);
        const long to = line + 1 < lineCount
            ? e->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line + 1)
            : length;
        const QByteArrayView text(document + from, to - from);

        startStyling(int(from));
        bool afterDot = false; // In o.name, name is a column, not a keyword or type.
        for (const sql::Token &t : sql::tokenize(text, state)) {
            Style style = styleFor(t, text);
            if (afterDot && (style == Keyword || style == UnreservedKeyword || style == Type))
                style = Identifier;
            if (sql::isSignificant(t.kind))
                afterDot = t.kind == TokenKind::Punctuation && text[t.offset] == '.';
            setStyling(int(t.length), style);
        }

        const int encoded = encode(state);
        const int previous = int(e->SendScintilla(QsciScintillaBase::SCI_GETLINESTATE, line));
        e->SendScintilla(QsciScintillaBase::SCI_SETLINESTATE, line, encoded);
        // Past the requested lines, go on only while this edit changes what
        // the following lines start with.
        if (line >= lastLine && encoded == previous)
            break;
    }
}

QColor SqlLexer::defaultColor(int style) const
{
    // Light and dark variants of a restrained palette.
    auto pick = [this](const char *light, const char *dark) {
        return QColor::fromString(QLatin1String(m_dark ? dark : light));
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
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (style == Keyword)
        font.setBold(true);
    if (style == Comment)
        font.setItalic(true);
    return font;
}

const char *SqlLexer::wordCharacters() const
{
    return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$";
}

} // namespace slonisko
