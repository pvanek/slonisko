// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ValueFormat.h"

#include <algorithm>

namespace slonisko::catalog {

namespace {

using Kind = ValueToken::Kind;

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

QByteArrayView trimmed(QByteArrayView text)
{
    while (!text.isEmpty() && isSpace(text.front()))
        text = text.sliced(1);
    while (!text.isEmpty() && isSpace(text.back()))
        text.chop(1);
    return text;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Skips digits from i; returns how many there were.
qsizetype digits(QByteArrayView text, qsizetype &i)
{
    const qsizetype start = i;
    while (i < text.size() && isDigit(text[i]))
        ++i;
    return i - start;
}

// JSON's own grammar: no leading zeros, no leading plus, no bare dot.
bool isJsonNumber(QByteArrayView word)
{
    qsizetype i = 0;
    if (i < word.size() && word[i] == '-')
        ++i;
    if (i < word.size() && word[i] == '0')
        ++i;
    else if (digits(word, i) == 0)
        return false;
    if (i < word.size() && word[i] == '.') {
        ++i;
        if (digits(word, i) == 0)
            return false;
    }
    if (i < word.size() && (word[i] == 'e' || word[i] == 'E')) {
        ++i;
        if (i < word.size() && (word[i] == '+' || word[i] == '-'))
            ++i;
        if (digits(word, i) == 0)
            return false;
    }
    return i == word.size();
}

// Looser, as array elements of any numeric type print: 1, -2.5, 1e-07,
// NaN, Infinity.
bool isArrayNumber(QByteArrayView word)
{
    if (word == "NaN" || word == "Infinity" || word == "-Infinity")
        return true;
    qsizetype i = 0;
    if (i < word.size() && (word[i] == '-' || word[i] == '+'))
        ++i;
    qsizetype mantissa = digits(word, i);
    if (i < word.size() && word[i] == '.') {
        ++i;
        mantissa += digits(word, i);
    }
    if (mantissa == 0)
        return false;
    if (i < word.size() && (word[i] == 'e' || word[i] == 'E')) {
        ++i;
        if (i < word.size() && (word[i] == '+' || word[i] == '-'))
            ++i;
        if (digits(word, i) == 0)
            return false;
    }
    return i == word.size();
}

bool isJsonDelimiter(char c)
{
    return isSpace(c) || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':'
        || c == '"';
}

// The punctuation character a token is, or 0 for any other token.
char punctuation(const std::vector<ValueToken> &tokens, QByteArrayView text, std::size_t k)
{
    return k < tokens.size() && tokens[k].kind == Kind::Punctuation ? text[tokens[k].start] : 0;
}

// Recursive descent over the tokens. Deeper than this is taken for not
// JSON rather than risking the stack; the value is then shown as it is.
class JsonParser
{
public:
    JsonParser(const std::vector<ValueToken> &tokens, QByteArrayView text)
        : m_tokens(tokens), m_text(text)
    { }

    bool document() { return value(0) && m_i == m_tokens.size(); }

private:
    static constexpr int MaxDepth = 1000;

    char punct() const { return punctuation(m_tokens, m_text, m_i); }

    bool value(int depth)
    {
        if (m_i >= m_tokens.size() || depth > MaxDepth)
            return false;
        const Kind kind = m_tokens[m_i].kind;
        if (kind == Kind::String || kind == Kind::Number || kind == Kind::Literal) {
            ++m_i;
            return true;
        }
        const char open = punct();
        if (open != '[' && open != '{')
            return false;
        const char close = open == '[' ? ']' : '}';
        ++m_i;
        if (punct() == close) {
            ++m_i;
            return true;
        }
        for (;;) {
            if (open == '{') {
                if (m_i >= m_tokens.size() || m_tokens[m_i].kind != Kind::Key)
                    return false;
                ++m_i;
                if (punct() != ':')
                    return false;
                ++m_i;
            }
            if (!value(depth + 1))
                return false;
            if (punct() == ',') {
                ++m_i;
                continue;
            }
            if (punct() != close)
                return false;
            ++m_i;
            return true;
        }
    }

    const std::vector<ValueToken> &m_tokens;
    QByteArrayView m_text;
    std::size_t m_i = 0;
};

bool isArrayType(Oid type)
{
    switch (type) {
    case 143: // xml[]
    case 199: // json[]
    case 651: // cidr[]
    case 791: // money[]
    case 1000: // bool[]
    case 1001: // bytea[]
    case 1002: // char[]
    case 1003: // name[]
    case 1005: // int2[]
    case 1007: // int4[]
    case 1009: // text[]
    case 1014: // bpchar[]
    case 1015: // varchar[]
    case 1016: // int8[]
    case 1021: // float4[]
    case 1022: // float8[]
    case 1028: // oid[]
    case 1034: // aclitem[]
    case 1040: // macaddr[]
    case 1041: // inet[]
    case 1115: // timestamp[]
    case 1182: // date[]
    case 1183: // time[]
    case 1185: // timestamptz[]
    case 1187: // interval[]
    case 1231: // numeric[]
    case 1263: // cstring[]
    case 1270: // timetz[]
    case 1561: // bit[]
    case 1563: // varbit[]
    case 2287: // record[]
    case 2951: // uuid[]
    case 3643: // tsvector[]
    case 3807: // jsonb[]
        return true;
    default:
        return false;
    }
}

// Types whose values are free text: what they hold is no hint of a type.
bool isTextType(Oid type)
{
    switch (type) {
    case 0:
    case 19: // name
    case 25: // text
    case 705: // unknown
    case 1042: // bpchar
    case 1043: // varchar
        return true;
    default:
        return false;
    }
}

} // namespace

std::vector<ValueToken> tokenizeJson(QByteArrayView text)
{
    std::vector<ValueToken> tokens;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const char c = text[i];
        if (isSpace(c)) {
            ++i;
            continue;
        }
        const qsizetype start = i;
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
            tokens.push_back({Kind::Punctuation, i++, 1});
            continue;
        }
        if (c == '"') {
            bool closed = false;
            for (++i; i < n && text[i] != '\n'; ++i) {
                if (text[i] == '\\') {
                    ++i;
                } else if (text[i] == '"') {
                    ++i;
                    closed = true;
                    break;
                }
            }
            i = std::min(i, n);
            Kind kind = closed ? Kind::String : Kind::Error;
            if (closed) {
                qsizetype next = i;
                while (next < n && isSpace(text[next]))
                    ++next;
                if (next < n && text[next] == ':')
                    kind = Kind::Key;
            }
            tokens.push_back({kind, start, i - start});
            continue;
        }
        while (i < n && !isJsonDelimiter(text[i]))
            ++i;
        if (i == start)
            ++i; // A character no rule takes, such as a NUL.
        const QByteArrayView word = text.sliced(start, i - start);
        const Kind kind = isJsonNumber(word)                      ? Kind::Number
            : word == "true" || word == "false" || word == "null" ? Kind::Literal
                                                                  : Kind::Error;
        tokens.push_back({kind, start, i - start});
    }
    return tokens;
}

std::vector<ValueToken> tokenizeArray(QByteArrayView text)
{
    std::vector<ValueToken> tokens;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n && isSpace(text[i]))
        ++i;
    // Bounds other than 1 come first: [0:2]={1,2,3}.
    if (i < n && text[i] == '[') {
        const qsizetype equals = text.indexOf('=', i);
        const qsizetype end = equals < 0 ? n : equals + 1;
        tokens.push_back({equals < 0 ? Kind::Error : Kind::Punctuation, i, end - i});
        i = end;
    }
    while (i < n) {
        const char c = text[i];
        if (isSpace(c)) {
            ++i;
            continue;
        }
        const qsizetype start = i;
        // A box[] separates its elements with semicolons.
        if (c == '{' || c == '}' || c == ',' || c == ';') {
            tokens.push_back({Kind::Punctuation, i++, 1});
            continue;
        }
        if (c == '"') {
            bool closed = false;
            for (++i; i < n; ++i) {
                if (text[i] == '\\') {
                    ++i;
                } else if (text[i] == '"') {
                    ++i;
                    closed = true;
                    break;
                }
            }
            i = std::min(i, n);
            tokens.push_back({closed ? Kind::String : Kind::Error, start, i - start});
            continue;
        }
        while (i < n && text[i] != '{' && text[i] != '}' && text[i] != ',' && text[i] != ';'
               && text[i] != '"')
            ++i;
        const QByteArrayView word = trimmed(text.sliced(start, i - start));
        const Kind kind = word.compare("NULL", Qt::CaseInsensitive) == 0 ? Kind::Literal
            : isArrayNumber(word)                                        ? Kind::Number
                                                                         : Kind::String;
        tokens.push_back({kind, start, word.size()});
    }
    return tokens;
}

std::vector<ValueToken> tokenize(ValueSyntax syntax, QByteArrayView text)
{
    switch (syntax) {
    case ValueSyntax::Json:
        return tokenizeJson(text);
    case ValueSyntax::Array:
        return tokenizeArray(text);
    case ValueSyntax::Text:
        break;
    }
    return {};
}

bool isValidJson(QByteArrayView text)
{
    const std::vector<ValueToken> tokens = tokenizeJson(text);
    return JsonParser(tokens, text).document();
}

std::optional<QByteArray> prettyJson(QByteArrayView text)
{
    const std::vector<ValueToken> tokens = tokenizeJson(text);
    if (!JsonParser(tokens, text).document())
        return std::nullopt;

    QByteArray out;
    out.reserve(text.size() * 2);
    int depth = 0;
    auto newLine = [&] {
        out += '\n';
        out.append(depth * 4, ' ');
    };
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        const ValueToken &token = tokens[k];
        if (token.kind != Kind::Punctuation) {
            out += text.sliced(token.start, token.length);
            continue;
        }
        switch (const char c = text[token.start]) {
        case '{':
        case '[':
            out += c;
            if (const char close = c == '{' ? '}' : ']';
                punctuation(tokens, text, k + 1) == close) {
                out += close; // Empty: {} and [] as they are.
                ++k;
            } else {
                ++depth;
                newLine();
            }
            break;
        case '}':
        case ']':
            --depth;
            newLine();
            out += c;
            break;
        case ',':
            out += c;
            newLine();
            break;
        case ':':
            out += ": ";
            break;
        }
    }
    return out;
}

std::optional<QByteArray> prettyArray(QByteArrayView text)
{
    const std::vector<ValueToken> tokens = tokenizeArray(text);
    std::size_t first = 0;
    if (!tokens.empty() && text[tokens[0].start] == '[')
        first = 1;
    if (punctuation(tokens, text, first) != '{')
        return std::nullopt;
    // Whether the braces match before formatting anything.
    int open = 0;
    for (std::size_t k = first; k < tokens.size(); ++k) {
        if (tokens[k].kind == Kind::Error || (open == 0 && k > first))
            return std::nullopt;
        const char c = punctuation(tokens, text, k);
        open += c == '{' ? 1 : c == '}' ? -1 : 0;
    }
    if (open != 0)
        return std::nullopt;

    QByteArray out;
    out.reserve(text.size() * 2);
    if (first == 1)
        out += text.sliced(tokens[0].start, tokens[0].length);
    std::vector<bool> breaks; // Per open brace: its elements on lines of their own.
    int depth = 0;
    auto newLine = [&] {
        out += '\n';
        out.append(depth * 4, ' ');
    };
    for (std::size_t k = first; k < tokens.size(); ++k) {
        const ValueToken &token = tokens[k];
        const char c = punctuation(tokens, text, k);
        if (c == '{') {
            out += c;
            if (punctuation(tokens, text, k + 1) == '}') {
                out += '}';
                ++k;
                continue;
            }
            const bool lines = breaks.empty() || punctuation(tokens, text, k + 1) == '{';
            breaks.push_back(lines);
            if (lines) {
                ++depth;
                newLine();
            }
        } else if (c == '}') {
            if (breaks.back()) {
                --depth;
                newLine();
            }
            breaks.pop_back();
            out += c;
        } else if (c == ',' || c == ';') {
            out += c;
            if (breaks.back())
                newLine();
        } else {
            out += text.sliced(token.start, token.length);
        }
    }
    return out;
}

std::optional<QByteArray> pretty(ValueSyntax syntax, QByteArrayView text)
{
    switch (syntax) {
    case ValueSyntax::Json:
        return prettyJson(text);
    case ValueSyntax::Array:
        return prettyArray(text);
    case ValueSyntax::Text:
        break;
    }
    return std::nullopt;
}

ValueSyntax guessSyntax(Oid type, QByteArrayView value)
{
    if (type == 114 || type == 3802) // json, jsonb
        return ValueSyntax::Json;
    if (isArrayType(type))
        return ValueSyntax::Array;
    const QByteArrayView v = trimmed(value);
    if (v.isEmpty())
        return ValueSyntax::Text;
    // A JSON document kept in a text column, as plenty of schemas do.
    if ((v.front() == '{' || v.front() == '[') && isValidJson(v))
        return ValueSyntax::Json;
    // An array of an enum, a domain or another type of the database's own.
    if (!isTextType(type) && v.front() == '{' && v.back() == '}' && prettyArray(v))
        return ValueSyntax::Array;
    return ValueSyntax::Text;
}

} // namespace slonisko::catalog
