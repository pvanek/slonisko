// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Scope.h"

#include "sql/Keywords.h"

#include <QJsonDocument>

#include <pg_query.h>

namespace slonisko::catalog::scope {

QString normalize(QByteArrayView text, TokenKind kind)
{
    QString s = QString::fromUtf8(text);
    if (kind == TokenKind::QuotedIdentifier) {
        if (s.startsWith(QLatin1String("U&")))
            s = s.mid(2);
        if (s.startsWith(QLatin1Char('"')))
            s = s.mid(1);
        if (s.endsWith(QLatin1Char('"')))
            s.chop(1);
        return s.replace(QStringLiteral("\"\""), QStringLiteral("\""));
    }
    return s.toLower();
}

bool isName(TokenKind kind)
{
    return kind == TokenKind::Identifier || kind == TokenKind::QuotedIdentifier
        || kind == TokenKind::Keyword;
}

QString quoted(const QString &name)
{
    if (!sql::needsQuoting(name.toUtf8()))
        return name;
    return QLatin1Char('"') + QString(name).replace(QLatin1Char('"'), QStringLiteral("\"\""))
        + QLatin1Char('"');
}

bool endsFromItem(const Tokens &t, int i)
{
    return t.isAnyOf(i, {"where",       "join",    "inner",   "left",      "right",   "full",
                         "cross",       "natural", "on",      "using",     "group",   "order",
                         "having",      "limit",   "offset",  "window",    "union",   "except",
                         "intersect",   "set",     "values",  "returning", "for",     "fetch",
                         "tablesample", "select",  "lateral", "when",      "default", "do",
                         "into"});
}

bool canBeAlias(const Tokens &t, int i)
{
    if (i >= t.size() || endsFromItem(t, i))
        return false;
    const TokenKind kind = t.at(i).kind;
    if (kind == TokenKind::Identifier || kind == TokenKind::QuotedIdentifier)
        return true;
    return kind == TokenKind::Keyword
        && sql::keywordCategory(t.text(i)) == sql::KeywordCategory::Unreserved;
}

Scope tokenScope(const Tokens &t)
{
    Scope scope;
    for (int i = 0; i < t.size(); ++i) {
        if (t.is(i, "with")) {
            int j = i + 1;
            if (t.is(j, "recursive"))
                ++j;
            while (j < t.size() && isName(t.at(j).kind)) {
                Source cte;
                cte.name = t.name(j);
                cte.derived = true;
                ++j;
                if (t.isPunct(j, '(')) {
                    const int end = t.skipParens(j);
                    for (int k = j + 1; k < end - 1; ++k) {
                        if (isName(t.at(k).kind))
                            cte.columns << t.name(k);
                    }
                    j = end;
                }
                if (!t.is(j, "as"))
                    break;
                ++j;
                if (t.is(j, "not"))
                    ++j;
                if (t.is(j, "materialized"))
                    ++j;
                if (t.isPunct(j, '('))
                    j = t.skipParens(j);
                scope.ctes.push_back(cte);
                if (!t.isPunct(j, ','))
                    break;
                ++j;
            }
            continue;
        }
        if (!t.isAnyOf(i, {"from", "join", "update", "into", "using"}))
            continue;
        const bool list = t.is(i, "from");
        // INSERT INTO t (a, b) and UPDATE t: a column list, not a function call.
        const bool functions = !t.isAnyOf(i, {"into", "update"});
        int j = i + 1;
        while (j < t.size()) {
            if (t.isAnyOf(j, {"only", "lateral"}))
                ++j;
            Source source;
            if (t.isPunct(j, '(')) {
                source.derived = true;
                j = t.skipParens(j);
            } else if (j < t.size() && isName(t.at(j).kind) && !endsFromItem(t, j)) {
                QStringList parts {t.name(j)};
                int last = j;
                ++j;
                while (t.isPunct(j, '.') && j + 1 < t.size() && isName(t.at(j + 1).kind)) {
                    parts << t.name(j + 1);
                    last = j + 1;
                    j += 2;
                }
                source.relation = parts.last();
                source.offset = t.at(last).offset;
                source.length = t.at(last).length;
                if (parts.size() > 1)
                    source.schema = parts[parts.size() - 2];
                source.name = source.relation;
                if (functions && t.isPunct(j, '(')) { // A function in FROM.
                    source.derived = true;
                    source.schema.clear();
                    j = t.skipParens(j);
                }
            } else {
                break;
            }
            if (t.is(j, "as"))
                ++j;
            if (canBeAlias(t, j)) {
                source.name = t.name(j);
                ++j;
                if (t.isPunct(j, '(')) {
                    const int end = t.skipParens(j);
                    for (int k = j + 1; k < end - 1; ++k) {
                        if (isName(t.at(k).kind))
                            source.columns << t.name(k);
                    }
                    j = end;
                }
            }
            if (!source.name.isEmpty())
                scope.sources.push_back(source);
            if (!(list && t.isPunct(j, ',')))
                break;
            ++j;
        }
    }
    return scope;
}

QStringList stringList(const QJsonArray &items)
{
    QStringList out;
    for (const QJsonValue &v : items) {
        const QJsonObject s = v.toObject().value(QLatin1String("String")).toObject();
        if (!s.isEmpty())
            out << s.value(QLatin1String("sval")).toString();
    }
    return out;
}

QStringList targetNames(const QJsonObject &selectStmt)
{
    QStringList out;
    for (const QJsonValue &v : selectStmt.value(QLatin1String("targetList")).toArray()) {
        const QJsonObject target = v.toObject().value(QLatin1String("ResTarget")).toObject();
        QString name = target.value(QLatin1String("name")).toString();
        if (name.isEmpty()) {
            const QJsonObject ref = target.value(QLatin1String("val"))
                                        .toObject()
                                        .value(QLatin1String("ColumnRef"))
                                        .toObject();
            const QStringList fields = stringList(ref.value(QLatin1String("fields")).toArray());
            if (!fields.isEmpty())
                name = fields.last();
        }
        if (!name.isEmpty())
            out << name;
    }
    return out;
}

Source rangeVarSource(const QJsonObject &rv)
{
    Source s;
    s.offset = rv.contains(QLatin1String("location"))
        ? rv.value(QLatin1String("location")).toInteger()
        : -1;
    s.schema = rv.value(QLatin1String("schemaname")).toString();
    s.relation = rv.value(QLatin1String("relname")).toString();
    const QString alias
        = rv.value(QLatin1String("alias")).toObject().value(QLatin1String("aliasname")).toString();
    s.name = alias.isEmpty() ? s.relation : alias;
    return s;
}

QJsonObject unwrap(const QJsonObject &value, const char *node)
{
    const QJsonValue wrapped = value.value(QLatin1String(node));
    return wrapped.isObject() ? wrapped.toObject() : value;
}

std::optional<QJsonObject> parse(const QByteArray &sql)
{
    PgQueryParseResult result = pg_query_parse(sql.constData());
    std::optional<QJsonObject> tree;
    if (!result.error)
        tree = QJsonDocument::fromJson(QByteArray(result.parse_tree)).object();
    pg_query_free_parse_result(result);
    return tree;
}

} // namespace slonisko::catalog::scope
