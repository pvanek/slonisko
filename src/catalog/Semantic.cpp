// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Semantic.h"

#include "catalog/Scope.h"
#include "sql/EmbeddedLexer.h"
#include "sql/Splitter.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <ranges>

namespace slonisko::catalog {

namespace {

using namespace scope;
using Kind = SemanticSpan::Kind;

// Top-level statements whose relations must exist, so a missing one is
// worth pointing out. DDL creates or names objects that need not exist yet.
bool queriesData(const QString &statement)
{
    return statement == QLatin1String("SelectStmt") || statement == QLatin1String("InsertStmt")
        || statement == QLatin1String("UpdateStmt") || statement == QLatin1String("DeleteStmt")
        || statement == QLatin1String("MergeStmt");
}

QString relationDetail(const Relation &r)
{
    const char *kind = r.kind == 'v' ? "view"
        : r.kind == 'm'              ? "materialized view"
        : r.kind == 'f'              ? "foreign table"
        : r.kind == 'p'              ? "partitioned table"
                                     : "table";
    return QStringLiteral("%1 %2.%3, %4 columns")
        .arg(QLatin1String(kind), r.schema, r.name)
        .arg(r.columns.size());
}

const Column *findColumn(const Relation &r, const QString &name)
{
    for (const Column &c : r.columns) {
        if (c.name == name)
            return &c;
    }
    return nullptr;
}

// Names the script creates with CREATE TABLE, VIEW and the like, so that
// queries on them further down are not taken for mistakes.
QSet<QString> createdNames(const QByteArray &script, const std::vector<sql::StatementSpan> &spans)
{
    QSet<QString> out;
    for (const sql::StatementSpan &span : spans) {
        if (span.kind != sql::StatementSpan::Kind::Sql)
            continue;
        const Tokens t(QByteArrayView(script).sliced(span.offset, span.length));
        int i = 0;
        if (t.is(0, "create")) {
            i = 1;
            while (t.isAnyOf(i,
                             {"or", "replace", "temp", "temporary", "unlogged", "global", "local",
                              "materialized", "recursive", "foreign"}))
                ++i;
            if (!t.isAnyOf(i, {"table", "view", "sequence"}))
                continue;
            ++i;
            if (t.is(i, "if"))
                i += 3; // IF NOT EXISTS
        } else if (t.is(0, "select")) {
            // SELECT ... INTO [TEMP] [TABLE] name
            while (i < t.size() && !t.is(i, "into") && !t.is(i, "from"))
                ++i;
            if (!t.is(i, "into"))
                continue;
            ++i;
            while (t.isAnyOf(i, {"temp", "temporary", "unlogged", "table"}))
                ++i;
        } else {
            continue;
        }
        int last = i;
        while (t.isPunct(last + 1, '.') && last + 2 < t.size())
            last += 2;
        if (last < t.size() && isName(t.at(last).kind))
            out.insert(t.name(last));
    }
    return out;
}

// Names referenced in a parse tree, with their byte locations.
struct References
{
    struct RangeVarRef
    {
        QString schema;
        QString relation;
        qsizetype location = -1;
        bool created = false; // The target of CREATE TABLE, SELECT INTO and the like.
    };
    struct ColumnRefRef
    {
        QStringList fields; // Without a trailing *.
        bool star = false;
        qsizetype location = -1;
    };
    struct FuncCallRef
    {
        QStringList names;
        qsizetype location = -1;
    };

    QString statement; // Type of the top-level node, like "SelectStmt".
    std::vector<RangeVarRef> rangeVars;
    std::vector<ColumnRefRef> columns;
    std::vector<FuncCallRef> functions;
    Scope scope;
    QString language; // Of CREATE FUNCTION or DO.
};

class ReferenceWalker
{
public:
    explicit ReferenceWalker(References &refs) : m_refs(refs) { }

    void walkStatement(const QJsonObject &tree)
    {
        const QJsonObject stmt = tree.value(QLatin1String("stmts"))
                                     .toArray()
                                     .first()
                                     .toObject()
                                     .value(QLatin1String("stmt"))
                                     .toObject();
        if (!stmt.isEmpty())
            m_refs.statement = stmt.begin().key();
        walk(stmt, QString());
    }

private:
    // Fields typed as a specific node are not wrapped like generic ones.
    static QString typedField(const QString &key)
    {
        if (key == QLatin1String("relation") || key == QLatin1String("rel")
            || key == QLatin1String("view") || key == QLatin1String("sequence"))
            return QStringLiteral("RangeVar");
        if (key == QLatin1String("intoClause") || key == QLatin1String("into"))
            return QStringLiteral("IntoClause");
        return {};
    }

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
            const QString typed = !node && it.value().isObject() ? typedField(k) : QString();
            if (node || !typed.isEmpty()) {
                const QString type = node ? k : typed;
                m_path.push_back({type, key, k});
                visit(type, it.value().toObject());
                walk(it.value(), node ? k : key);
                m_path.pop_back();
            } else {
                walk(it.value(), k);
            }
        }
    }

    // Whether the RangeVar being visited names a new object: IntoClause.rel
    // (SELECT INTO, CREATE TABLE AS), ViewStmt.view, CreateSeqStmt.sequence
    // or CreateStmt.relation, not the relations the statement reads.
    bool naming() const
    {
        const Frame &self = m_path.back();
        if (self.field == QLatin1String("rel") || self.field == QLatin1String("view")
            || self.field == QLatin1String("sequence"))
            return true;
        return self.field == QLatin1String("relation") && m_path.size() >= 2
            && m_path[m_path.size() - 2].node == QLatin1String("CreateStmt");
    }

    void visit(const QString &type, const QJsonObject &n)
    {
        if (type == QLatin1String("RangeVar")) {
            References::RangeVarRef ref;
            ref.schema = n.value(QLatin1String("schemaname")).toString();
            ref.relation = n.value(QLatin1String("relname")).toString();
            ref.location = n.value(QLatin1String("location")).toInteger(-1);
            ref.created = naming();
            m_refs.rangeVars.push_back(ref);
            if (!ref.created)
                m_refs.scope.sources.push_back(rangeVarSource(n));
        } else if (type == QLatin1String("RangeSubselect")
                   || type == QLatin1String("RangeFunction")) {
            const QJsonObject alias = n.value(QLatin1String("alias")).toObject();
            Source s;
            s.name = alias.value(QLatin1String("aliasname")).toString();
            s.derived = true;
            s.columns = stringList(alias.value(QLatin1String("colnames")).toArray());
            if (s.columns.isEmpty() && type == QLatin1String("RangeSubselect"))
                s.columns = targetNames(
                    unwrap(n.value(QLatin1String("subquery")).toObject(), "SelectStmt"));
            if (s.columns.isEmpty() && type == QLatin1String("RangeFunction"))
                s.columns << s.name;
            if (!s.name.isEmpty())
                m_refs.scope.sources.push_back(s);
        } else if (type == QLatin1String("CommonTableExpr")) {
            Source s;
            s.name = n.value(QLatin1String("ctename")).toString();
            s.derived = true;
            s.columns = stringList(n.value(QLatin1String("aliascolnames")).toArray());
            if (s.columns.isEmpty())
                s.columns = targetNames(
                    unwrap(n.value(QLatin1String("ctequery")).toObject(), "SelectStmt"));
            m_refs.scope.ctes.push_back(s);
        } else if (type == QLatin1String("ColumnRef")) {
            References::ColumnRefRef ref;
            const QJsonArray fields = n.value(QLatin1String("fields")).toArray();
            ref.star
                = !fields.isEmpty() && fields.last().toObject().contains(QLatin1String("A_Star"));
            ref.fields = stringList(fields);
            ref.location = n.value(QLatin1String("location")).toInteger(-1);
            m_refs.columns.push_back(ref);
        } else if (type == QLatin1String("FuncCall")) {
            References::FuncCallRef ref;
            ref.names = stringList(n.value(QLatin1String("funcname")).toArray());
            ref.location = n.value(QLatin1String("location")).toInteger(-1);
            m_refs.functions.push_back(ref);
        } else if (type == QLatin1String("DefElem")
                   && n.value(QLatin1String("defname")).toString() == QLatin1String("language")) {
            m_refs.language = n.value(QLatin1String("arg"))
                                  .toObject()
                                  .value(QLatin1String("String"))
                                  .toObject()
                                  .value(QLatin1String("sval"))
                                  .toString();
        }
    }

    struct Frame
    {
        QString node;
        QString key; // The key of the parent that holds the node's list or value.
        QString field; // The key the node itself sits under.
    };

    References &m_refs;
    std::vector<Frame> m_path;
};

class Analyzer
{
public:
    Analyzer(QByteArrayView statement, qsizetype base, const Snapshot *snapshot,
             const QSet<QString> &created, std::vector<SemanticSpan> &out)
        : m_sql(statement), m_base(base), m_snapshot(snapshot), m_created(created), m_out(out),
          m_tokens(statement)
    { }

    void run()
    {
        // psql's :name would keep the statement from parsing; a placeholder
        // of the same length keeps every offset where it was.
        QByteArray parseable = m_sql.toByteArray();
        for (const sql::Token &t : sql::tokenize(m_sql)) {
            if (t.kind == TokenKind::PsqlVariable && !t.inBody)
                parseable.replace(t.offset, t.length, '1' + QByteArray(t.length - 1, ' '));
        }
        const std::optional<QJsonObject> tree = parse(parseable);
        References refs;
        if (tree)
            ReferenceWalker(refs).walkStatement(*tree);
        foreignBody(tree ? refs.language : tokenLanguage());
        if (!m_snapshot)
            return;
        if (tree)
            fromTree(refs);
        else
            fromTokens();
    }

private:
    void add(Kind kind, qsizetype offset, qsizetype length, const QString &detail = {},
             const Relation *relation = nullptr)
    {
        if (offset >= 0 && length > 0) {
            m_out.push_back({kind, m_base + offset, length, detail,
                             relation ? relation->oid : Oid(0),
                             relation ? relation->kind : char(0)});
        }
    }

    // The significant token starting at a byte offset, or -1.
    int tokenAt(qsizetype offset) const
    {
        for (int i = 0; i < m_tokens.size(); ++i) {
            if (m_tokens.at(i).offset == offset)
                return i;
            if (m_tokens.at(i).offset > offset)
                break;
        }
        return -1;
    }

    // The tokens of a dotted name starting at token i: a, a.b or a.b.c.
    std::vector<int> nameParts(int i) const
    {
        std::vector<int> parts;
        while (i >= 0 && i < m_tokens.size() && isName(m_tokens.at(i).kind)) {
            parts.push_back(i);
            if (!m_tokens.isPunct(i + 1, '.'))
                break;
            i += 2;
        }
        return parts;
    }

    // LANGUAGE x of a function or DO block that does not parse.
    QString tokenLanguage() const
    {
        for (int i = 0; i + 1 < m_tokens.size(); ++i) {
            if (m_tokens.is(i, "language") && isName(m_tokens.at(i + 1).kind))
                return m_tokens.name(i + 1);
        }
        return {};
    }

    // Highlights the body of a function or DO block in another language.
    void foreignBody(const QString &declared)
    {
        const bool function = m_tokens.is(0, "create")
            && std::ranges::any_of(std::views::iota(0, std::min(m_tokens.size(), 6)), [&](int i) {
                                  return m_tokens.isAnyOf(i, {"function", "procedure"});
                              });
        const bool block = m_tokens.is(0, "do");
        if (!function && !block)
            return;
        const QString language = declared.isEmpty()
            ? (block ? QStringLiteral("plpgsql") : QStringLiteral("sql"))
            : declared;
        const sql::EmbeddedLanguage lang = sql::embeddedLanguage(language);
        if (lang == sql::EmbeddedLanguage::Sql)
            return;

        // The body: what the lexer took for SQL inside dollar quotes, or a
        // plain string after AS.
        qsizetype from = -1, to = -1;
        for (const sql::Token &t : sql::tokenize(m_sql)) {
            if (t.inBody) {
                if (from < 0)
                    from = t.offset;
                to = t.end();
            }
        }
        bool dollarQuoted = from >= 0;
        if (from < 0) {
            for (int i = 0; i + 1 < m_tokens.size(); ++i) {
                const sql::Token &next = m_tokens.at(i + 1);
                if (m_tokens.is(i, "as")
                    && (next.kind == TokenKind::String || next.kind == TokenKind::DollarString)) {
                    const bool quote = next.kind == TokenKind::String;
                    from = next.offset + (quote ? 1 : 0);
                    to = next.end() - (quote ? 1 : 0);
                    dollarQuoted = !quote;
                    break;
                }
            }
        }
        if (from < 0 || to <= from)
            return;
        // Unknown languages: only a dollar-quoted body is worth showing as
        // plain text; C's AS 'library', 'symbol' strings are fine as they are.
        if (lang == sql::EmbeddedLanguage::Other && !dollarQuoted)
            return;
        add(Kind::ForeignText, from, to - from, language);
        using EK = sql::EmbeddedToken::Kind;
        for (const sql::EmbeddedToken &t :
             sql::tokenizeEmbedded(m_sql.sliced(from, to - from), lang)) {
            const Kind kind = t.kind == EK::Keyword ? Kind::ForeignKeyword
                : t.kind == EK::String              ? Kind::ForeignString
                : t.kind == EK::Comment             ? Kind::ForeignComment
                                                    : Kind::ForeignNumber;
            add(kind, from + t.offset, t.length);
        }
    }

    bool isCte(const Scope &scope, const QString &name) const
    {
        return std::ranges::any_of(scope.ctes, [&](const Source &s) { return s.name == name; });
    }

    void fromTree(const References &refs)
    {
        const bool flag = queriesData(refs.statement);
        m_scope = refs.scope;

        for (const auto &ref : refs.rangeVars) {
            const std::vector<int> parts = nameParts(tokenAt(ref.location));
            if (parts.empty())
                continue;
            const sql::Token &name = m_tokens.at(parts.back());
            if (ref.schema.isEmpty() && isCte(refs.scope, ref.relation)) {
                add(Kind::Cte, name.offset, name.length, QStringLiteral("WITH query"));
            } else if (const Relation *r = m_snapshot->findRelation(ref.schema, ref.relation)) {
                add(Kind::Relation, name.offset, name.length, relationDetail(*r), r);
            } else if (flag && !ref.created && !m_created.contains(ref.relation)) {
                add(Kind::UnknownRelation, name.offset, name.length,
                    QStringLiteral("Not in the catalog loaded when connecting: %1")
                        .arg(ref.relation));
            }
        }

        for (const auto &ref : refs.columns)
            column(ref.fields, nameParts(tokenAt(ref.location)));

        for (const auto &ref : refs.functions) {
            const std::vector<int> parts = nameParts(tokenAt(ref.location));
            if (parts.empty() || ref.names.isEmpty())
                continue;
            const QString schema
                = ref.names.size() > 1 ? ref.names[ref.names.size() - 2] : QString();
            if (const Function *f = findFunction(schema, ref.names.last())) {
                const sql::Token &name = m_tokens.at(parts.back());
                add(Kind::Function, name.offset, name.length,
                    QStringLiteral("%1.%2(%3) → %4")
                        .arg(f->schema, f->name, f->arguments, f->result));
            }
        }
    }

    const Function *findFunction(const QString &schema, const QString &name) const
    {
        for (const Function &f : m_snapshot->functions) {
            if (f.name == name
                && (schema.isEmpty() ? m_snapshot->searchPath.contains(f.schema)
                                     : f.schema == schema))
                return &f;
        }
        return nullptr;
    }

    // The sources a name refers to: relations with that alias or name.
    std::vector<const Source *> sourcesNamed(const QString &name) const
    {
        std::vector<const Source *> out;
        auto add = [&](const Source *s) {
            if (std::ranges::find(out, s) == out.end())
                out.push_back(s);
        };
        for (const Source &s : m_scope.sources) {
            if (s.name != name)
                continue;
            // FROM r, where r is a WITH query: that query.
            const Source *cte = s.schema.isEmpty() ? findCte(s.relation) : nullptr;
            add(cte ? cte : &s);
        }
        if (out.empty()) {
            if (const Source *cte = findCte(name))
                add(cte);
        }
        return out;
    }

    const Source *findCte(const QString &name) const
    {
        for (const Source &s : m_scope.ctes) {
            if (s.name == name)
                return &s;
        }
        return nullptr;
    }

    const Relation *relationOf(const Source &s) const
    {
        if (s.derived || (s.schema.isEmpty() && isCte(m_scope, s.relation)))
            return nullptr;
        return m_snapshot->findRelation(s.schema, s.relation);
    }

    // A column reference: its fields and the tokens they are in.
    void column(const QStringList &fields, const std::vector<int> &parts)
    {
        if (fields.isEmpty() || parts.size() < std::size_t(fields.size()))
            return;
        const sql::Token &token = m_tokens.at(parts[std::size_t(fields.size()) - 1]);
        const QString name = fields.last();

        if (fields.size() == 1) {
            // Unqualified: a column of any relation in scope, if one has it.
            for (const Source &s : m_scope.sources) {
                if (const Relation *r = relationOf(s)) {
                    if (const Column *c = findColumn(*r, name)) {
                        add(Kind::Column, token.offset, token.length,
                            QStringLiteral("%1 · %2.%3").arg(c->type, r->name, c->name));
                        return;
                    }
                } else if (s.columns.contains(name)) {
                    add(Kind::Column, token.offset, token.length, s.name);
                    return;
                }
            }
            return; // Maybe an output alias or a parameter: leave it be.
        }

        const QString qualifier = fields[fields.size() - 2];
        const Relation *r = nullptr;
        if (fields.size() == 2) {
            const std::vector<const Source *> named = sourcesNamed(qualifier);
            if (named.size() != 1)
                return; // Unknown or ambiguous.
            if (named.front()->derived || !relationOf(*named.front())) {
                if (named.front()->columns.contains(name))
                    add(Kind::Column, token.offset, token.length, named.front()->name);
                return;
            }
            r = relationOf(*named.front());
        } else {
            r = m_snapshot->findRelation(fields[fields.size() - 3], qualifier);
        }
        if (!r)
            return;
        if (const Column *c = findColumn(*r, name))
            add(Kind::Column, token.offset, token.length,
                QStringLiteral("%1 · %2.%3").arg(c->type, r->name, c->name));
        else
            add(Kind::UnknownColumn, token.offset, token.length,
                QStringLiteral("%1.%2 has no column %3").arg(r->schema, r->name, name));
    }

    // For statements that do not parse: relations named after FROM and
    // the like, and alias.column pairs. Never marks anything unknown.
    void fromTokens()
    {
        m_scope = tokenScope(m_tokens);
        for (const Source &s : m_scope.sources) {
            if (s.offset < 0)
                continue;
            if (s.schema.isEmpty() && isCte(m_scope, s.relation))
                add(Kind::Cte, s.offset, s.length, QStringLiteral("WITH query"));
            else if (const Relation *r = relationOf(s))
                add(Kind::Relation, s.offset, s.length, relationDetail(*r), r);
        }
        for (int i = 0; i + 2 < m_tokens.size(); ++i) {
            if (!isName(m_tokens.at(i).kind) || !m_tokens.isPunct(i + 1, '.')
                || !isName(m_tokens.at(i + 2).kind) || m_tokens.isPunct(i - 1, '.'))
                continue;
            const std::vector<const Source *> named = sourcesNamed(m_tokens.name(i));
            if (named.size() != 1)
                continue;
            const Relation *r = relationOf(*named.front());
            const Column *c = r ? findColumn(*r, m_tokens.name(i + 2)) : nullptr;
            if (c) {
                const sql::Token &t = m_tokens.at(i + 2);
                add(Kind::Column, t.offset, t.length,
                    QStringLiteral("%1 · %2.%3").arg(c->type, r->name, c->name));
            }
        }
    }

    QByteArrayView m_sql;
    qsizetype m_base;
    const Snapshot *m_snapshot;
    const QSet<QString> &m_created;
    std::vector<SemanticSpan> &m_out;
    Tokens m_tokens;
    Scope m_scope;
};

} // namespace

std::vector<SemanticSpan> analyzeScript(const QByteArray &script, const Snapshot *snapshot,
                                        qsizetype from, qsizetype to)
{
    if (to < 0)
        to = script.size();
    const auto spans = sql::splitStatements(script);
    const QSet<QString> created = snapshot ? createdNames(script, spans) : QSet<QString>();

    std::vector<SemanticSpan> out;
    for (const sql::StatementSpan &span : spans) {
        if (span.kind != sql::StatementSpan::Kind::Sql || span.offset + span.length < from
            || span.offset > to)
            continue;
        Analyzer(QByteArrayView(script).sliced(span.offset, span.length), span.offset, snapshot,
                 created, out)
            .run();
    }
    std::ranges::stable_sort(out, {}, &SemanticSpan::offset);
    return out;
}

} // namespace slonisko::catalog
