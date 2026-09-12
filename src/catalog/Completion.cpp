// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Completion.h"

#include "catalog/Scope.h"
#include "sql/Keywords.h"
#include "sql/Lexer.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace slonisko::catalog {

namespace {

using Context = Completion::Context;
using Item = CompletionItem;
using sql::Token;
using sql::TokenKind;
using namespace scope;

// Put in place of the word at the cursor so the statement can parse.
const QString Sentinel = QStringLiteral("slonisko_cursor");

constexpr int MaxItems = 200;

// What the parse tree says about the cursor and the scope.
struct TreeInfo
{
    Context context = Context::Keyword;
    QStringList qualifiers;
    std::optional<Source> target; // INSERT INTO or UPDATE relation, for their column lists.
    bool targetColumns = false; // The cursor is in such a column list.
    Scope scope;
};

// Parse tree walking (pg_query's JSON).

class TreeWalker
{
public:
    explicit TreeWalker(TreeInfo &info) : m_info(info) { }

    void walk(const QJsonValue &value, const QString &key)
    {
        if (value.isArray()) {
            for (const QJsonValue &v : value.toArray())
                walk(v, key);
            return;
        }
        if (!value.isObject())
            return;
        const QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QString &k = it.key();
            const bool node = !k.isEmpty() && k[0].isUpper() && it.value().isObject();
            // Fields typed as a specific node, like InsertStmt.relation, are
            // not wrapped in {"RangeVar": ...} like generic Node fields are.
            const QString typed = !node && it.value().isObject() ? typedField(k) : QString();
            if (node || !typed.isEmpty()) {
                const QString type = node ? k : typed;
                m_stack.push_back({type, key});
                visit(type, it.value().toObject());
                walk(it.value(), node ? k : key);
                m_stack.pop_back();
            } else {
                if (k == QLatin1String("aliasname") && it.value().toString() == Sentinel)
                    m_info.context = Context::Keyword; // Typing after a name: a keyword.
                walk(it.value(), k);
            }
        }
    }

private:
    static QString typedField(const QString &key)
    {
        if (key == QLatin1String("relation"))
            return QStringLiteral("RangeVar");
        if (key == QLatin1String("typeName"))
            return QStringLiteral("TypeName");
        return {};
    }

    struct Frame
    {
        QString node;
        QString key; // Under which key of its parent the node sits.
    };

    bool inside(const char *node, const char *key) const
    {
        // The innermost frame below the ResTarget itself.
        if (m_stack.size() < 2)
            return false;
        const Frame &target = m_stack.back();
        const Frame &parent = m_stack[m_stack.size() - 2];
        return parent.node == QLatin1String(node) && target.key == QLatin1String(key);
    }

    void visit(const QString &type, const QJsonObject &n)
    {
        if (type == QLatin1String("RangeVar")) {
            if (n.value(QLatin1String("relname")).toString() == Sentinel) {
                m_info.context = Context::Relation;
                const QString schema = n.value(QLatin1String("schemaname")).toString();
                m_info.qualifiers = schema.isEmpty() ? QStringList() : QStringList {schema};
            } else {
                m_info.scope.sources.push_back(rangeVarSource(n));
            }
        } else if (type == QLatin1String("RangeSubselect")) {
            const QJsonObject alias = n.value(QLatin1String("alias")).toObject();
            Source s;
            s.name = alias.value(QLatin1String("aliasname")).toString();
            s.derived = true;
            s.columns = stringList(alias.value(QLatin1String("colnames")).toArray());
            if (s.columns.isEmpty())
                s.columns = targetNames(n.value(QLatin1String("subquery"))
                                            .toObject()
                                            .value(QLatin1String("SelectStmt"))
                                            .toObject());
            if (!s.name.isEmpty() && s.name != Sentinel)
                m_info.scope.sources.push_back(s);
        } else if (type == QLatin1String("RangeFunction")) {
            const QJsonObject alias = n.value(QLatin1String("alias")).toObject();
            Source s;
            s.name = alias.value(QLatin1String("aliasname")).toString();
            s.derived = true;
            s.columns = stringList(alias.value(QLatin1String("colnames")).toArray());
            if (s.columns.isEmpty() && !s.name.isEmpty())
                s.columns << s.name; // A scalar function's single column takes the alias.
            if (!s.name.isEmpty() && s.name != Sentinel)
                m_info.scope.sources.push_back(s);
        } else if (type == QLatin1String("CommonTableExpr")) {
            Source s;
            s.name = n.value(QLatin1String("ctename")).toString();
            s.derived = true;
            s.columns = stringList(n.value(QLatin1String("aliascolnames")).toArray());
            if (s.columns.isEmpty())
                s.columns = targetNames(n.value(QLatin1String("ctequery"))
                                            .toObject()
                                            .value(QLatin1String("SelectStmt"))
                                            .toObject());
            m_info.scope.ctes.push_back(s);
        } else if (type == QLatin1String("ColumnRef")) {
            const QStringList fields = stringList(n.value(QLatin1String("fields")).toArray());
            if (!fields.isEmpty() && fields.last() == Sentinel) {
                m_info.context = fields.size() > 1 ? Context::Qualified : Context::Column;
                m_info.qualifiers = fields.mid(0, fields.size() - 1);
            }
        } else if (type == QLatin1String("TypeName")) {
            const QStringList names = stringList(n.value(QLatin1String("names")).toArray());
            if (names.contains(Sentinel)) {
                m_info.context = Context::Type;
                m_info.qualifiers = names.mid(0, names.indexOf(Sentinel));
                if (m_info.qualifiers == QStringList {QStringLiteral("pg_catalog")})
                    m_info.qualifiers.clear(); // Added by the parser to built-in types.
            }
        } else if (type == QLatin1String("FuncCall")) {
            const QStringList names = stringList(n.value(QLatin1String("funcname")).toArray());
            if (!names.isEmpty() && names.last() == Sentinel) {
                m_info.context = Context::Function;
                m_info.qualifiers = names.mid(0, names.size() - 1);
            }
        } else if (type == QLatin1String("InsertStmt") || type == QLatin1String("UpdateStmt")) {
            m_info.target = rangeVarSource(n.value(QLatin1String("relation")).toObject());
        } else if (type == QLatin1String("ResTarget")
                   && n.value(QLatin1String("name")).toString() == Sentinel) {
            if (inside("InsertStmt", "cols") || inside("UpdateStmt", "targetList")) {
                m_info.context = Context::Column;
                m_info.targetColumns = true;
            } else {
                m_info.context = Context::Keyword; // SELECT a |: an alias or FROM.
            }
        } else if (type == QLatin1String("ColumnDef")
                   && n.value(QLatin1String("colname")).toString() == Sentinel) {
            m_info.context = Context::None; // Naming a new column.
        }
    }

    TreeInfo &m_info;
    std::vector<Frame> m_stack;
};

std::optional<TreeInfo> parseWithSentinel(const QByteArray &sql)
{
    const std::optional<QJsonObject> tree = scope::parse(sql);
    if (!tree || !QJsonDocument(*tree).toJson(QJsonDocument::Compact).contains(Sentinel.toUtf8()))
        return std::nullopt;
    TreeInfo info;
    TreeWalker(info).walk(*tree, QString());
    return info;
}

QByteArray closingParens(const QByteArray &sql)
{
    int depth = 0;
    for (const Token &t : sql::tokenize(sql)) {
        if (t.kind != TokenKind::Punctuation)
            continue;
        if (sql[t.offset] == '(')
            ++depth;
        else if (sql[t.offset] == ')' && depth > 0)
            --depth;
    }
    return QByteArray(depth, ')');
}

// Whether the cursor is inside a comment or literal, where nothing completes.
bool insideLiteral(const Token &t, qsizetype cursor, const QByteArray &sql)
{
    if (t.kind != TokenKind::Comment && t.kind != TokenKind::String
        && !(t.kind == TokenKind::DollarString && !t.inBody))
        return false;
    if (cursor < t.end())
        return true;
    const QByteArrayView text = QByteArrayView(sql).sliced(t.offset, t.length);
    if (text.startsWith("--"))
        return true; // Runs to the end of the line.
    // At the end of an unterminated string or comment: still inside it.
    if (t.kind == TokenKind::Comment)
        return !text.endsWith("*/");
    if (t.kind == TokenKind::String)
        return text.size() < 2 || !text.endsWith("'") || text.endsWith("\\'");
    return true; // Before the closing delimiter, which is a token of its own.
}

// Guesses the context from the tokens before the cursor.
Context heuristicContext(const Tokens &t, int wordStart, bool qualified, bool *insertColumns)
{
    if (qualified)
        return Context::Qualified;
    int prev = -1; // Last token before the word.
    for (int i = 0; i < t.size() && t.at(i).offset < wordStart; ++i)
        prev = i;
    if (prev < 0)
        return Context::Keyword;

    auto valueLike = [&](int i) {
        const TokenKind k = t.at(i).kind;
        return k == TokenKind::Identifier || k == TokenKind::QuotedIdentifier
            || k == TokenKind::Number || k == TokenKind::String || k == TokenKind::Parameter
            || t.isPunct(i, ')')
            || (k == TokenKind::Keyword
                && sql::keywordCategory(t.text(i)) == sql::KeywordCategory::Unreserved);
    };

    int depth = 0;
    for (int i = prev; i >= 0; --i) {
        if (t.isPunct(i, ')')) {
            ++depth;
            continue;
        }
        if (t.isPunct(i, '(')) {
            if (depth > 0) {
                --depth;
                continue;
            }
            // Inside parentheses: INSERT INTO t ( ... ) takes t's columns.
            if (i >= 2 && isName(t.at(i - 1).kind) && t.is(i - 2, "into")) {
                *insertColumns = true;
                return Context::Column;
            }
            if (i == prev)
                return Context::Column; // Right after "(": a function argument or a subexpression.
            continue;
        }
        if (depth > 0)
            continue;
        if (t.isAnyOf(i,
                      {"from", "join", "update", "into", "table", "truncate", "only", "lateral"})) {
            if (i == prev || (t.isPunct(prev, ',') && t.is(i, "from")))
                return Context::Relation;
            return Context::Keyword; // After a name: an alias, or WHERE and the like.
        }
        if (t.isAnyOf(i,
                      {"select", "where", "on", "by", "having", "set", "returning", "when", "then",
                       "else", "and", "or", "not", "case", "distinct", "values", "in", "using"})) {
            return i == prev || !valueLike(prev) ? Context::Column : Context::Keyword;
        }
    }
    return valueLike(prev) ? Context::Keyword : Context::Column;
}

// How much more likely common keywords are than the rest, roughly by how
// often they are typed.
int keywordWeight(const QString &word)
{
    static const QHash<QString, int> weights {
        {QStringLiteral("select"), 40},    {QStringLiteral("from"), 40},
        {QStringLiteral("where"), 40},     {QStringLiteral("join"), 38},
        {QStringLiteral("and"), 38},       {QStringLiteral("or"), 36},
        {QStringLiteral("order"), 36},     {QStringLiteral("group"), 36},
        {QStringLiteral("by"), 36},        {QStringLiteral("limit"), 34},
        {QStringLiteral("on"), 34},        {QStringLiteral("as"), 34},
        {QStringLiteral("left"), 32},      {QStringLiteral("inner"), 30},
        {QStringLiteral("having"), 30},    {QStringLiteral("insert"), 30},
        {QStringLiteral("into"), 30},      {QStringLiteral("values"), 30},
        {QStringLiteral("update"), 30},    {QStringLiteral("set"), 30},
        {QStringLiteral("delete"), 30},    {QStringLiteral("not"), 30},
        {QStringLiteral("null"), 30},      {QStringLiteral("is"), 30},
        {QStringLiteral("in"), 30},        {QStringLiteral("like"), 28},
        {QStringLiteral("ilike"), 26},     {QStringLiteral("distinct"), 28},
        {QStringLiteral("union"), 26},     {QStringLiteral("offset"), 26},
        {QStringLiteral("returning"), 26}, {QStringLiteral("with"), 26},
        {QStringLiteral("create"), 26},    {QStringLiteral("alter"), 24},
        {QStringLiteral("drop"), 24},      {QStringLiteral("table"), 24},
        {QStringLiteral("view"), 22},      {QStringLiteral("index"), 22},
        {QStringLiteral("between"), 22},   {QStringLiteral("exists"), 22},
        {QStringLiteral("asc"), 22},       {QStringLiteral("desc"), 22},
        {QStringLiteral("cross"), 20},     {QStringLiteral("full"), 20},
        {QStringLiteral("right"), 20},     {QStringLiteral("outer"), 20},
        {QStringLiteral("using"), 20},     {QStringLiteral("true"), 20},
        {QStringLiteral("false"), 20},     {QStringLiteral("case"), 20},
        {QStringLiteral("when"), 18},      {QStringLiteral("then"), 18},
        {QStringLiteral("else"), 18},      {QStringLiteral("end"), 18},
    };
    return weights.value(word, 0);
}

Item::Kind relationKind(char relkind)
{
    switch (relkind) {
    case 'v':
        return Item::Kind::View;
    case 'm':
        return Item::Kind::MaterializedView;
    case 'f':
        return Item::Kind::ForeignTable;
    default:
        return Item::Kind::Table;
    }
}

// Collects candidates, scoring them against the prefix.
class Candidates
{
public:
    explicit Candidates(const QString &prefix) : m_prefix(prefix) { }

    void add(Item::Kind kind, const QString &label, const QString &insert, const QString &detail,
             int weight)
    {
        int match = fuzzyScore(m_prefix, label);
        if (match == 0)
            return;
        if (!m_prefix.isEmpty() && label.startsWith(m_prefix))
            match += 10; // The same case as typed, as for quoted names.
        const QString key = QString::number(int(kind)) + QLatin1Char(':') + label;
        if (m_seen.contains(key))
            return;
        m_seen.insert(key);
        m_items.push_back({kind, label, insert, detail, match + weight});
    }

    std::vector<Item> take()
    {
        std::ranges::stable_sort(m_items, [](const Item &a, const Item &b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
        });
        if (m_items.size() > MaxItems)
            m_items.resize(MaxItems);
        return std::move(m_items);
    }

private:
    QString m_prefix;
    QSet<QString> m_seen;
    std::vector<Item> m_items;
};

bool isSystemSchema(const QString &schema)
{
    return schema == QLatin1String("pg_catalog") || schema == QLatin1String("information_schema");
}

QString relationDetail(const Relation &r)
{
    const char *kind = r.kind == 'v' ? "view"
        : r.kind == 'm'              ? "materialized view"
        : r.kind == 'f'              ? "foreign table"
                                     : "table";
    return r.schema + QLatin1Char(' ') + QLatin1String(kind);
}

class Completer
{
public:
    Completer(const QByteArray &statement, qsizetype cursor, const Snapshot &snapshot)
        : m_sql(statement), m_cursor(std::clamp<qsizetype>(cursor, 0, statement.size())),
          m_snapshot(snapshot)
    { }

    Completion run()
    {
        Completion out;
        out.replaceFrom = out.replaceTo = m_cursor;

        // The word at the cursor, or where one would start.
        const std::vector<Token> all = sql::tokenize(m_sql);
        TokenKind wordKind = TokenKind::Unknown;
        for (const Token &t : all) {
            if (t.offset < m_cursor && m_cursor <= t.end()) {
                if (insideLiteral(t, m_cursor, m_sql)) {
                    out.context = Context::None;
                    return out;
                }
                if (t.inBody)
                    return inBody(all);
                if (isName(t.kind)) {
                    out.replaceFrom = t.offset;
                    out.replaceTo = t.end();
                    wordKind = t.kind;
                }
            }
        }
        QByteArrayView typed
            = QByteArrayView(m_sql).sliced(out.replaceFrom, m_cursor - out.replaceFrom);
        if (wordKind == TokenKind::QuotedIdentifier) {
            while (!typed.isEmpty()
                   && (typed.front() == 'U' || typed.front() == '&' || typed.front() == '"'))
                typed = typed.sliced(1);
        }
        out.prefix = QString::fromUtf8(typed);

        // Qualifiers before the word: "schema." or "alias." or "schema.table.".
        const Tokens tokens(m_sql);
        QStringList qualifiers;
        {
            int i = -1;
            for (int k = 0; k < tokens.size() && tokens.at(k).offset < out.replaceFrom; ++k)
                i = k;
            while (i >= 1 && tokens.isPunct(i, '.') && tokens.at(i).end() <= out.replaceFrom
                   && isName(tokens.at(i - 1).kind)) {
                qualifiers.prepend(tokens.name(i - 1));
                i -= 2;
            }
        }

        // The parser knows best; try variants that often make a half-typed
        // statement parse.
        const QByteArray before = m_sql.left(out.replaceFrom) + Sentinel.toUtf8();
        const QByteArray full = before + m_sql.mid(out.replaceTo);
        std::optional<TreeInfo> tree;
        for (const QByteArray &attempt :
             {full, full + closingParens(full), before + closingParens(before)}) {
            if ((tree = parseWithSentinel(attempt)))
                break;
        }

        Scope scope = tokenScope(tokens);
        bool insertColumns = false;
        std::optional<Source> target;
        if (tree) {
            out.context = tree->context;
            if (!tree->qualifiers.isEmpty())
                qualifiers = tree->qualifiers;
            // The tree's scope wins; tokens add what it missed.
            for (const Source &s : scope.sources) {
                if (std::ranges::none_of(tree->scope.sources,
                                         [&](const Source &t) { return t.name == s.name; }))
                    tree->scope.sources.push_back(s);
            }
            for (const Source &s : scope.ctes) {
                if (std::ranges::none_of(tree->scope.ctes,
                                         [&](const Source &t) { return t.name == s.name; }))
                    tree->scope.ctes.push_back(s);
            }
            scope = tree->scope;
            insertColumns = tree->targetColumns;
            target = tree->target;
        } else {
            out.context = heuristicContext(tokens, int(out.replaceFrom), !qualifiers.isEmpty(),
                                           &insertColumns);
            if (insertColumns) {
                // INSERT INTO t ( ...: the relation named after INTO.
                for (const Source &s : scope.sources) {
                    target = s;
                    break;
                }
            }
        }
        m_scope = scope;

        const bool lowercase = !out.prefix.isEmpty() && out.prefix == out.prefix.toLower();
        Candidates candidates(out.prefix);
        switch (out.context) {
        case Context::None:
            break;
        case Context::Keyword:
            addKeywords(candidates, lowercase, 0);
            break;
        case Context::Relation:
            addRelations(candidates, qualifiers.value(0));
            break;
        case Context::Qualified:
            addQualified(candidates, qualifiers);
            break;
        case Context::Type:
            addTypes(candidates, qualifiers.value(0));
            break;
        case Context::Function:
            addFunctions(candidates, qualifiers.value(0), 0);
            break;
        case Context::Column:
            if (insertColumns && target) {
                addColumnsOf(candidates, *target, 100);
                break;
            }
            for (const Source &s : m_scope.sources)
                addColumnsOf(candidates, s, 100);
            for (const Source &s : m_scope.sources) // To qualify: "o" then ".".
                candidates.add(
                    Item::Kind::Alias, s.name, quoted(s.name),
                    s.relation.isEmpty() || s.relation == s.name ? QString() : s.relation, 40);
            addFunctions(candidates, {}, 0);
            if (!out.prefix.isEmpty())
                addKeywords(candidates, lowercase, -60);
            break;
        }
        out.items = candidates.take();
        return out;
    }

private:
    // Inside a function or DO body: complete within the body's current
    // statement, lexed and parsed on its own.
    Completion inBody(const std::vector<Token> &all)
    {
        qsizetype from = 0, to = m_sql.size();
        for (const Token &t : all) {
            const bool boundary = t.kind == TokenKind::DollarDelimiter
                || (t.inBody && t.kind == TokenKind::Punctuation && m_sql[t.offset] == ';');
            if (!boundary)
                continue;
            if (t.end() <= m_cursor)
                from = t.end();
            else if (t.offset >= m_cursor) {
                to = t.offset;
                break;
            }
        }
        Completion inner = Completer(m_sql.mid(from, to - from), m_cursor - from, m_snapshot).run();
        inner.replaceFrom += from;
        inner.replaceTo += from;
        return inner;
    }

    const Source *findSource(const QString &name) const
    {
        for (const Source &s : m_scope.sources) {
            if (s.name == name)
                return &s;
        }
        for (const Source &s : m_scope.ctes) {
            if (s.name == name)
                return &s;
        }
        return nullptr;
    }

    void addColumnsOf(Candidates &c, const Source &s, int weight)
    {
        const QString where = s.name;
        // A CTE or subquery: its known columns.
        const Source *cte
            = s.schema.isEmpty() ? findCte(s.relation.isEmpty() ? s.name : s.relation) : nullptr;
        const QStringList derived = s.derived ? s.columns : cte ? cte->columns : QStringList();
        if (s.derived || cte) {
            for (const QString &column : derived)
                c.add(Item::Kind::Column, column, quoted(column), where, weight);
            return;
        }
        if (const Relation *r = m_snapshot.findRelation(s.schema, s.relation)) {
            for (const Column &column : r->columns)
                c.add(Item::Kind::Column, column.name, quoted(column.name),
                      column.type + QStringLiteral(" · ") + where, weight);
        }
    }

    const Source *findCte(const QString &name) const
    {
        for (const Source &s : m_scope.ctes) {
            if (s.name == name)
                return &s;
        }
        return nullptr;
    }

    void addQualified(Candidates &c, const QStringList &qualifiers)
    {
        const QString last = qualifiers.last();
        // alias. or table. or schema.table.
        if (qualifiers.size() == 1) {
            if (const Source *s = findSource(last)) {
                addColumnsOf(c, *s, 100);
                return;
            }
        }
        const QString schema
            = qualifiers.size() >= 2 ? qualifiers[qualifiers.size() - 2] : QString();
        if (const Relation *r = m_snapshot.findRelation(schema, last)) {
            Source s;
            s.schema = r->schema;
            s.relation = s.name = r->name;
            addColumnsOf(c, s, 100);
            return;
        }
        if (qualifiers.size() == 1 && m_snapshot.hasSchema(last)) {
            addRelations(c, last);
            addFunctions(c, last, -20);
        }
    }

    void addRelations(Candidates &c, const QString &schema)
    {
        for (const Relation &r : m_snapshot.relations) {
            if (schema.isEmpty() ? !m_snapshot.searchPath.contains(r.schema) : r.schema != schema)
                continue;
            c.add(relationKind(r.kind), r.name, quoted(r.name), relationDetail(r),
                  isSystemSchema(r.schema) ? 0 : 50);
        }
        if (schema.isEmpty()) {
            for (const Source &cte : m_scope.ctes)
                c.add(Item::Kind::Cte, cte.name, quoted(cte.name), QStringLiteral("WITH query"),
                      60);
            for (const QString &s : m_snapshot.schemas)
                c.add(Item::Kind::Schema, s, quoted(s), QStringLiteral("schema"),
                      isSystemSchema(s) ? 0 : 20);
        }
    }

    void addFunctions(Candidates &c, const QString &schema, int weight)
    {
        for (const Function &f : m_snapshot.functions) {
            if (schema.isEmpty() ? !m_snapshot.searchPath.contains(f.schema) : f.schema != schema)
                continue;
            c.add(Item::Kind::Function, f.name, quoted(f.name),
                  f.name + QLatin1Char('(') + f.arguments + QStringLiteral(") → ") + f.result,
                  weight);
        }
    }

    void addTypes(Candidates &c, const QString &schema)
    {
        for (const Type &t : m_snapshot.types) {
            if (schema.isEmpty() ? !m_snapshot.searchPath.contains(t.schema) : t.schema != schema)
                continue;
            // format_type() already quotes and qualifies where needed.
            c.add(Item::Kind::Type, t.name, t.name, t.schema, isSystemSchema(t.schema) ? 20 : 50);
        }
        if (schema.isEmpty()) {
            for (const QString &s : m_snapshot.schemas)
                c.add(Item::Kind::Schema, s, quoted(s), QStringLiteral("schema"), 0);
        }
    }

    void addKeywords(Candidates &c, bool lowercase, int weight)
    {
        for (const QByteArray &k : sql::keywords()) {
            const QString word = QString::fromLatin1(k);
            c.add(Item::Kind::Keyword, lowercase ? word : word.toUpper(),
                  lowercase ? word : word.toUpper(), QString(), weight + keywordWeight(word));
        }
    }

    const QByteArray m_sql;
    const qsizetype m_cursor;
    const Snapshot &m_snapshot;
    Scope m_scope;
};

} // namespace

namespace {

// How well typed matches candidate, ignoring which tier they are in: 0 to
// 100. Matches are looked for in every position, not greedily, so a later
// run of letters can win over an early stray one: "leapa" takes "lea" and
// "pa" out of "learning_package" rather than stopping at the first "a".
int alignmentQuality(const QString &typed, const QString &candidate)
{
    constexpr int Match = 16; // Any letter in the right order.
    constexpr int WordStart = 18; // learning_[p]ackage, Order[I]tems.
    constexpr int Consecutive = 12; // Right after the previous match.
    constexpr int GapPenalty = 2; // Per letter skipped, up to the bonuses.
    constexpr int MaxPerChar = Match + WordStart + Consecutive;

    const qsizetype n = typed.size(), m = candidate.size();
    auto wordStart = [&](qsizetype i) {
        if (i == 0)
            return true;
        const QChar before = candidate[i - 1];
        return before == QLatin1Char('_') || before == QLatin1Char(' ')
            || before == QLatin1Char('.') || (candidate[i].isUpper() && before.isLower());
    };

    // best[j]: the best score for the typed letters so far, the last of them
    // matched at candidate[j]. Unreachable positions stay at NoMatch.
    constexpr int NoMatch = std::numeric_limits<int>::min() / 2;
    std::vector<int> best(std::size_t(m), NoMatch), previous(std::size_t(m), NoMatch);
    for (qsizetype i = 0; i < n; ++i) {
        previous.swap(best);
        std::ranges::fill(best, NoMatch);
        // The best of the previous letter's matches, as it moves right.
        int carried = NoMatch;
        for (qsizetype j = 0; j < m; ++j) {
            if (j > 0) {
                if (carried > NoMatch)
                    carried -= GapPenalty;
                carried = std::max(carried, i == 0 ? 0 : previous[std::size_t(j - 1)]);
            } else if (i == 0) {
                carried = 0;
            }
            if (candidate[j].toLower() != typed[i].toLower() || carried <= NoMatch)
                continue;
            int score = carried + Match;
            if (wordStart(j))
                score += WordStart;
            if (i > 0 && j > 0 && previous[std::size_t(j - 1)] > NoMatch
                && previous[std::size_t(j - 1)] == carried)
                score += Consecutive; // The letter before it matched too.
            best[std::size_t(j)] = score;
        }
    }
    const auto top = std::ranges::max_element(best);
    if (top == best.end() || *top <= NoMatch)
        return 0;
    return std::clamp(100 * *top / int(MaxPerChar * n), 1, 100);
}

} // namespace

int fuzzyScore(const QString &typed, const QString &candidate)
{
    if (typed.isEmpty())
        return 1;
    const QString t = typed.toLower();
    const QString c = candidate.toLower();
    if (c.startsWith(t))
        return std::max(800, 1000 - int(c.size() - t.size()));

    const int quality = alignmentQuality(t, candidate);
    if (quality == 0)
        return 0; // Not even out of order: no match at all.

    // Word starts: after _, space or . and at camelCase humps.
    auto wordStart = [&](qsizetype i) {
        if (i == 0)
            return true;
        const QChar before = candidate[i - 1];
        return before == QLatin1Char('_') || before == QLatin1Char(' ')
            || before == QLatin1Char('.') || (candidate[i].isUpper() && before.isLower());
    };
    // An acronym, every letter starting a word: cnm for customer_name.
    qsizetype at = 0;
    bool acronym = true;
    for (const QChar ch : t) {
        qsizetype found = -1;
        for (qsizetype i = at; i < c.size() && found < 0; ++i)
            if (c[i] == ch && wordStart(i))
                found = i;
        if (found < 0) {
            acronym = false;
            break;
        }
        at = found + 1;
    }
    if (acronym)
        return 600 + quality;
    if (const qsizetype substring = c.indexOf(t); substring >= 0)
        return 500 - int(substring);
    // Letters in order but scattered: how well they line up decides.
    return 300 + quality;
}

Completion complete(const QByteArray &statement, qsizetype cursor, const Snapshot &snapshot)
{
    return Completer(statement, cursor, snapshot).run();
}

} // namespace slonisko::catalog
