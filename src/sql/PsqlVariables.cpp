// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/PsqlVariables.h"

#include "sql/Lexer.h"

namespace slonisko::sql {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
        || static_cast<unsigned char>(c) >= 0x80;
}

// As PQescapeLiteral: 'it''s', and E'...' with doubled backslashes when
// there are any, so it works whatever standard_conforming_strings is.
QString quoteLiteral(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    if (!value.contains(QLatin1Char('\\')))
        return QLatin1Char('\'') + escaped + QLatin1Char('\'');
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return QStringLiteral(" E'") + escaped + QLatin1Char('\'');
}

QString quoteIdentifier(const QString &value)
{
    return QLatin1Char('"') + QString(value).replace(QLatin1Char('"'), QStringLiteral("\"\""))
        + QLatin1Char('"');
}

// The name and form of a :name, :'name' or :"name" token.
struct Reference
{
    QString name;
    char quote = 0; // 0, ' or ".
};

Reference reference(QByteArrayView token)
{
    Reference r;
    QByteArrayView body = token.sliced(1);
    if (!body.isEmpty() && (body.front() == '\'' || body.front() == '"') && body.size() >= 2
        && body.back() == body.front()) {
        r.quote = body.front();
        body = body.sliced(1, body.size() - 2);
    }
    r.name = QString::fromUtf8(body);
    return r;
}

} // namespace

QStringList PsqlVariables::arguments(QByteArrayView s) const
{
    // Like psql's OT_NORMAL arguments: 'single quotes' are removed and
    // their escapes processed, "double quotes" are kept, and :name is
    // replaced outside quotes.
    QStringList out;
    qsizetype i = 0;
    const qsizetype n = s.size();
    while (i < n) {
        while (i < n && isSpace(s[i]))
            ++i;
        if (i >= n)
            break;
        QByteArray arg;
        while (i < n && !isSpace(s[i])) {
            const char c = s[i];
            if (c == '\'') {
                ++i;
                while (i < n) {
                    if (s[i] == '\'' && i + 1 < n && s[i + 1] == '\'') {
                        arg += '\'';
                        i += 2;
                    } else if (s[i] == '\'') {
                        ++i;
                        break;
                    } else if (s[i] == '\\' && i + 1 < n) {
                        const char e = s[i + 1];
                        arg += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                        i += 2;
                    } else {
                        arg += s[i++];
                    }
                }
            } else if (c == '"') {
                const qsizetype close = s.indexOf('"', i + 1);
                const qsizetype end = close < 0 ? n : close + 1;
                arg += s.sliced(i, end - i).toByteArray();
                i = end;
            } else if (c == ':' && i + 1 < n
                       && (isNameChar(s[i + 1]) || s[i + 1] == '\'' || s[i + 1] == '"')) {
                // :name, :'name' or :"name" inside an argument.
                qsizetype end = i + 1;
                if (s[end] == '\'' || s[end] == '"') {
                    const qsizetype close = s.indexOf(s[end], end + 1);
                    end = close < 0 ? n : close + 1;
                } else {
                    while (end < n && isNameChar(s[end]))
                        ++end;
                }
                const Reference ref = reference(s.sliced(i, end - i));
                if (m_values.contains(ref.name)) {
                    const QString v = m_values.value(ref.name);
                    arg += (ref.quote == '\''      ? quoteLiteral(v).trimmed()
                                : ref.quote == '"' ? quoteIdentifier(v)
                                                   : v)
                               .toUtf8();
                } else {
                    arg += s.sliced(i, end - i).toByteArray();
                }
                i = end;
            } else {
                arg += c;
                ++i;
            }
        }
        out << QString::fromUtf8(arg);
    }
    return out;
}

bool PsqlVariables::apply(const QByteArray &command, QString *output)
{
    QByteArrayView text(command);
    if (!text.startsWith('\\'))
        return false;
    qsizetype nameEnd = 1;
    while (nameEnd < text.size() && !isSpace(text[nameEnd]))
        ++nameEnd;
    const QByteArrayView name = text.sliced(1, nameEnd - 1);
    const QStringList args = arguments(text.sliced(nameEnd));

    if (name == "set") {
        if (args.isEmpty()) {
            QStringList lines;
            for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
                lines << it.key() + QStringLiteral(" = ") + quoteLiteral(it.value()).trimmed();
            if (output)
                *output = lines.join(QLatin1Char('\n'));
            return true;
        }
        m_values.insert(args.first(), args.mid(1).join(QString()));
        return true;
    }
    if (name == "unset") {
        for (const QString &arg : args)
            m_values.remove(arg);
        return true;
    }
    if (name == "echo") {
        QStringList words = args;
        if (!words.isEmpty() && words.first() == QLatin1String("-n"))
            words.removeFirst();
        if (output)
            *output = words.join(QLatin1Char(' '));
        return true;
    }
    return false;
}

QByteArray PsqlVariables::substitute(const QByteArray &sql, std::vector<Replacement> *replacements,
                                     QStringList *unset) const
{
    QByteArray out;
    qsizetype copied = 0;
    for (const Token &t : tokenize(sql)) {
        if (t.kind != TokenKind::PsqlVariable || t.inBody)
            continue;
        const Reference ref = reference(QByteArrayView(sql).sliced(t.offset, t.length));
        if (!m_values.contains(ref.name)) {
            if (unset && !unset->contains(ref.name))
                *unset << ref.name;
            continue;
        }
        const QString v = m_values.value(ref.name);
        const QByteArray value = (ref.quote == '\''      ? quoteLiteral(v)
                                      : ref.quote == '"' ? quoteIdentifier(v)
                                                         : v)
                                     .toUtf8();
        out += sql.mid(copied, t.offset - copied);
        out += value;
        copied = t.end();
        if (replacements)
            replacements->push_back({t.offset, t.length, value.size()});
    }
    out += sql.mid(copied);
    return out;
}

qsizetype PsqlVariables::originalOffset(qsizetype offset,
                                        const std::vector<Replacement> &replacements)
{
    qsizetype shift = 0; // Substituted minus original, before the offset.
    for (const Replacement &r : replacements) {
        const qsizetype start = r.offset + shift;
        if (offset < start)
            break;
        if (offset < start + r.newLength)
            return r.offset; // Inside a value: point at its :name.
        shift += r.newLength - r.length;
    }
    return offset - shift;
}

} // namespace slonisko::sql
