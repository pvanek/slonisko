// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Dependencies.h"

#include "catalog/Scope.h"
#include "pg/Result.h"
#include "sql/Keywords.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <pg_query.h>

#include <algorithm>
#include <deque>
#include <map>

namespace slonisko::catalog {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("slonisko::catalog", text);
}

// The catalogs' own oids, which are the same in every PostgreSQL.
constexpr Oid PgType = 1247;
constexpr Oid PgProc = 1255;
constexpr Oid PgClass = 1259;
constexpr Oid PgAttrdef = 2604;
constexpr Oid PgConstraint = 2606;
constexpr Oid PgRewrite = 2618;
constexpr Oid PgTrigger = 2620;
constexpr Oid PgNamespace = 2615;
constexpr Oid PgExtension = 3079;
constexpr Oid PgPolicy = 3256;
// Objects below it were made by initdb.
constexpr Oid FirstNormalObjectId = 16384;

quint64 keyOf(Oid classId, Oid oid)
{
    return (quint64(classId) << 32) | oid;
}

QString text(const pg::Result &result, int row, int column)
{
    return result.isNull(row, column) ? QString() : QString::fromUtf8(result.value(row, column));
}

Oid oidAt(const pg::Result &result, int row, int column)
{
    return Oid(result.value(row, column).toUInt());
}

// Every row of pg_depend that one user object has. A column default is
// reported as the column it belongs to; pinned rows say nothing, and
// those of initdb's objects only matter among themselves.
const char *const EdgesQuery
    = "WITH d AS ("
      "  SELECT CASE WHEN d.classid = 'pg_attrdef'::regclass::oid "
      "              THEN 'pg_class'::regclass::oid ELSE d.classid END AS classid, "
      "         COALESCE(ad.adrelid, d.objid) AS objid, "
      "         COALESCE(ad.adnum::int, d.objsubid) AS objsubid, "
      "         ad.oid IS NOT NULL AS via_default, "
      "         d.refclassid, d.refobjid, d.refobjsubid, d.deptype "
      "  FROM pg_depend d "
      "  LEFT JOIN pg_attrdef ad ON d.classid = 'pg_attrdef'::regclass::oid AND ad.oid = d.objid "
      "  WHERE d.objid >= 16384 AND d.deptype <> 'p') "
      "SELECT d.classid, d.objid, d.refclassid, d.refobjid, d.deptype::text, d.via_default, "
      "       da.attname, ra.attname "
      "FROM d "
      "LEFT JOIN pg_attribute da ON d.classid = 'pg_class'::regclass::oid AND d.objsubid > 0 "
      "  AND da.attrelid = d.objid AND da.attnum = d.objsubid "
      "LEFT JOIN pg_attribute ra ON d.refclassid = 'pg_class'::regclass::oid "
      "  AND d.refobjsubid > 0 AND ra.attrelid = d.refobjid AND ra.attnum = d.refobjsubid "
      "ORDER BY d.objid, d.refobjid, d.refobjsubid";

// Every object either side of those rows: what it is, what it is called.
// Every user object depends at least on its schema, so this is all of them.
const char *const ObjectsQuery
    = "WITH o AS ("
      "  SELECT classid, objid FROM pg_depend "
      "  WHERE objid >= 16384 AND deptype <> 'p' AND classid <> 'pg_attrdef'::regclass::oid "
      "  UNION SELECT refclassid, refobjid FROM pg_depend WHERE objid >= 16384 AND deptype <> 'p') "
      "SELECT o.classid, o.objid, i.type, i.schema, "
      "  COALESCE(c.relname::text, p.proname::text, t.typname::text, i.name), i.identity, "
      "  COALESCE(c.relkind::text, p.prokind::text, t.typtype::text, k.contype::text) "
      "FROM o CROSS JOIN LATERAL pg_identify_object(o.classid, o.objid, 0) i "
      "LEFT JOIN pg_class c ON o.classid = 'pg_class'::regclass::oid AND c.oid = o.objid "
      "LEFT JOIN pg_proc p ON o.classid = 'pg_proc'::regclass::oid AND p.oid = o.objid "
      "LEFT JOIN pg_type t ON o.classid = 'pg_type'::regclass::oid AND t.oid = o.objid "
      "LEFT JOIN pg_constraint k ON o.classid = 'pg_constraint'::regclass::oid AND k.oid = o.objid";

// Bodies pg_depend knows nothing about: SQL functions written as strings
// and PL/pgSQL, which is only looked at when it runs. Those of extensions
// are the extension's business.
const char *const BodiesQuery
    = "SELECT p.oid, l.lanname, p.prosrc, "
      "  (SELECT substr(c, 13) FROM unnest(p.proconfig) c "
      "   WHERE left(c, 12) = 'search_path=' LIMIT 1), "
      "  CASE WHEN l.lanname = 'plpgsql' THEN pg_get_functiondef(p.oid) END "
      "FROM pg_proc p JOIN pg_language l ON l.oid = p.prolang "
      "WHERE p.oid >= 16384 AND l.lanname IN ('sql', 'plpgsql') AND p.prosqlbody IS NULL "
      "  AND p.prokind IN ('f', 'p', 'w') "
      "  AND NOT EXISTS (SELECT FROM pg_depend e WHERE e.classid = 'pg_proc'::regclass::oid "
      "                  AND e.objid = p.oid AND e.deptype = 'e')";

// Where names in a body are looked up, unless the function sets its own.
const char *const SearchPathQuery = "SELECT current_user, s FROM unnest(current_schemas(true)) s";

const char *const ColumnsQuery = "SELECT attname FROM pg_attribute WHERE attrelid = %1 "
                                 "AND attnum > 0 AND NOT attisdropped ORDER BY attnum";

std::optional<Oid> catalogOf(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Table:
    case ObjectKind::PartitionedTable:
    case ObjectKind::View:
    case ObjectKind::MaterializedView:
    case ObjectKind::ForeignTable:
    case ObjectKind::Sequence:
    case ObjectKind::Index:
        return PgClass;
    case ObjectKind::Function:
    case ObjectKind::Procedure:
    case ObjectKind::Aggregate:
        return PgProc;
    case ObjectKind::Type:
    case ObjectKind::Domain:
        return PgType;
    case ObjectKind::Schema:
        return PgNamespace;
    case ObjectKind::Constraint:
        return PgConstraint;
    case ObjectKind::Extension:
        return PgExtension;
    default:
        return std::nullopt;
    }
}

bool hasColumns(ObjectKind kind)
{
    return kind == ObjectKind::Table || kind == ObjectKind::PartitionedTable
        || kind == ObjectKind::View || kind == ObjectKind::MaterializedView
        || kind == ObjectKind::ForeignTable;
}

// Body parsing.

QStringList stringsOf(const QJsonArray &items)
{
    QStringList out;
    for (const QJsonValue &v : items) {
        const QJsonObject s = v.toObject().value(QLatin1String("String")).toObject();
        if (!s.isEmpty())
            out << s.value(QLatin1String("sval")).toString();
    }
    return out;
}

std::optional<QualifiedName> nameOf(const QStringList &parts)
{
    if (parts.isEmpty() || parts.size() > 3)
        return std::nullopt;
    // A database-qualified name is the same name here, or an error.
    QualifiedName name {parts.size() > 1 ? parts[parts.size() - 2] : QString(), parts.last()};
    if (name.schema == QLatin1String("pg_catalog"))
        return std::nullopt;
    return name;
}

// A name written as text, like nextval('public.s') reads it: unquoted
// parts fold to lowercase, quoted ones keep their case.
QStringList splitQualified(const QString &text)
{
    QStringList parts;
    QString part;
    bool quoted = false;
    bool wasQuoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text[i + 1] == QLatin1Char('"')) {
                    part += c;
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                part += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = wasQuoted = true;
        } else if (c == QLatin1Char('.')) {
            parts << part;
            part.clear();
            wasQuoted = false;
        } else if (!c.isSpace()) {
            part += wasQuoted ? c : c.toLower();
        }
    }
    parts << part;
    return parts;
}

class Collector
{
public:
    explicit Collector(BodyReferences &out) : m_out(out) { }

    void walk(const QJsonValue &value, QStringView key = {})
    {
        if (value.isArray()) {
            for (const QJsonValue &item : value.toArray())
                walk(item);
            return;
        }
        if (!value.isObject())
            return;
        const QJsonObject object = value.toObject();
        // Typed fields hold their node bare, generic ones wrapped in its
        // name: InsertStmt.relation is a RangeVar, FromExpr items {"RangeVar": …}.
        if (key == QLatin1String("RangeVar") || key == QLatin1String("relation"))
            relation(object);
        else if (key == QLatin1String("TypeName") || key == QLatin1String("typeName"))
            type(object);
        else if (key == QLatin1String("FuncCall"))
            function(object);
        else if (key == QLatin1String("ColumnRef"))
            columnRef(object);
        else if (key == QLatin1String("ResTarget"))
            addColumn(object.value(QLatin1String("name")).toString());
        else if (key == QLatin1String("CommonTableExpr"))
            m_ctes.insert(object.value(QLatin1String("ctename")).toString());
        for (auto it = object.begin(); it != object.end(); ++it)
            walk(it.value(), it.key());
    }

    // Names that turned out to be the statement's own CTEs are no tables.
    void finish()
    {
        std::erase_if(m_out.relations, [this](const QualifiedName &name) {
            return name.schema.isEmpty() && m_ctes.contains(name.name);
        });
    }

    void addRelation(const QualifiedName &name)
    {
        if (!name.name.isEmpty()
            && std::ranges::find(m_out.relations, name) == m_out.relations.end())
            m_out.relations.push_back(name);
    }
    void addType(const QualifiedName &name)
    {
        if (!name.name.isEmpty() && std::ranges::find(m_out.types, name) == m_out.types.end())
            m_out.types.push_back(name);
    }

private:
    void relation(const QJsonObject &rv)
    {
        const QString name = rv.value(QLatin1String("relname")).toString();
        if (!name.isEmpty())
            addRelation({rv.value(QLatin1String("schemaname")).toString(), name});
    }

    void type(const QJsonObject &typeName)
    {
        if (const auto name = nameOf(stringsOf(typeName.value(QLatin1String("names")).toArray())))
            addType(*name);
    }

    void function(const QJsonObject &call)
    {
        const QStringList parts = stringsOf(call.value(QLatin1String("funcname")).toArray());
        const auto name = nameOf(parts);
        if (!name)
            return;
        if (std::ranges::find(m_out.functions, *name) == m_out.functions.end())
            m_out.functions.push_back(*name);
        // The sequence functions name their sequence in a string.
        static const QSet<QString> sequenceFunctions {
            QStringLiteral("nextval"), QStringLiteral("currval"), QStringLiteral("setval")};
        if (!name->schema.isEmpty() || !sequenceFunctions.contains(name->name))
            return;
        const QJsonArray args = call.value(QLatin1String("args")).toArray();
        if (args.isEmpty())
            return;
        QJsonObject arg = args.first().toObject();
        if (arg.contains(QLatin1String("TypeCast")))
            arg = arg.value(QLatin1String("TypeCast"))
                      .toObject()
                      .value(QLatin1String("arg"))
                      .toObject();
        const QString literal = arg.value(QLatin1String("A_Const"))
                                    .toObject()
                                    .value(QLatin1String("sval"))
                                    .toObject()
                                    .value(QLatin1String("sval"))
                                    .toString();
        if (!literal.isEmpty()) {
            if (const auto sequence = nameOf(splitQualified(literal)))
                addRelation(*sequence);
        }
    }

    void columnRef(const QJsonObject &ref)
    {
        const QJsonArray fields = ref.value(QLatin1String("fields")).toArray();
        if (fields.isEmpty())
            return;
        const QJsonObject last = fields.last().toObject();
        if (last.contains(QLatin1String("A_Star")))
            m_out.star = true;
        else
            addColumn(last.value(QLatin1String("String"))
                          .toObject()
                          .value(QLatin1String("sval"))
                          .toString());
    }

    void addColumn(const QString &name)
    {
        if (!name.isEmpty() && !m_out.columns.contains(name))
            m_out.columns << name;
    }

    BodyReferences &m_out;
    QSet<QString> m_ctes;
};

// One statement or expression, in the mode PL/pgSQL parsed it in.
bool collectSql(const QByteArray &sql, int mode, Collector &collector, QString *error)
{
    PgQueryParseResult result = pg_query_parse_opts(sql.constData(), mode);
    const bool ok = !result.error;
    if (ok)
        collector.walk(QJsonDocument::fromJson(QByteArray(result.parse_tree)).object());
    else if (error && error->isEmpty())
        *error = QString::fromUtf8(result.error->message);
    pg_query_free_parse_result(result);
    return ok;
}

// The datatype of a PL/pgSQL variable: "public.orders%rowtype" is a
// table's row, "orders.id%type" a column of one, anything else a type.
void collectPlpgsqlType(const QString &typname, Collector &collector)
{
    const qsizetype percent = typname.indexOf(QLatin1Char('%'));
    QString spelled = percent < 0 ? typname : typname.left(percent);
    spelled.remove(QStringLiteral("[]"));
    QStringList parts = splitQualified(spelled);
    if (percent >= 0) {
        if (typname.mid(percent + 1).compare(QLatin1String("type"), Qt::CaseInsensitive) == 0)
            parts.removeLast(); // The column; a variable's %TYPE leaves nothing.
        if (const auto name = nameOf(parts))
            collector.addRelation(*name);
    } else if (const auto name = nameOf(parts)) {
        collector.addType(*name);
    }
}

void walkPlpgsql(const QJsonValue &value, Collector &collector, QString *error, bool *ok)
{
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            walkPlpgsql(item, collector, error, ok);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QLatin1String("PLpgSQL_expr")) {
            const QJsonObject expr = it.value().toObject();
            // parseMode is libpg_query's own: plain statement, expression,
            // or one of the assignment forms.
            *ok &= collectSql(expr.value(QLatin1String("query")).toString().toUtf8(),
                              expr.value(QLatin1String("parseMode")).toInt(), collector, error);
        } else if (it.key() == QLatin1String("PLpgSQL_type")) {
            collectPlpgsqlType(it.value().toObject().value(QLatin1String("typname")).toString(),
                               collector);
        } else {
            walkPlpgsql(it.value(), collector, error, ok);
        }
    }
}

// Graph building.

bool isRelationKind(char kind)
{
    return kind == 'r' || kind == 'p' || kind == 'v' || kind == 'm' || kind == 'f' || kind == 'S';
}

bool isTableKind(char kind)
{
    return kind == 'r' || kind == 'p' || kind == 'f';
}

// Objects by unqualified name, for looking up what bodies name.
using NameIndex = QHash<QString, std::vector<int>>;

std::vector<int> resolve(const DependencyGraph &graph, const NameIndex &index,
                         const QualifiedName &name, const QStringList &path,
                         const QString &ownSchema)
{
    auto inSchema = [&](const QString &schema) {
        std::vector<int> found;
        for (int i : index.value(name.name)) {
            if (graph.objects[std::size_t(i)].schema == schema)
                found.push_back(i);
        }
        return found;
    };
    if (!name.schema.isEmpty())
        return inSchema(name.schema);
    for (const QString &schema : path) {
        if (std::vector<int> found = inSchema(schema); !found.empty())
            return found;
    }
    // Not where the server would look now: the function may well expect
    // to run with its own schema in the path, which is how it is usually
    // written, so that is a better guess than nothing.
    return inSchema(ownSchema);
}

int canonical(const std::vector<int> &owner, int i)
{
    for (int step = 0; step < 16 && owner[std::size_t(i)] != i; ++step)
        i = owner[std::size_t(i)];
    return i;
}

// Why the dependent needs what it is under, from the edges between them.
QString reasonFor(const DependencyGraph &graph, const std::vector<const DependencyEdge *> &edges)
{
    const DependencyObject &d = graph.objects[std::size_t(edges.front()->dependent)];
    const DependencyObject &r = graph.objects[std::size_t(edges.front()->referenced)];
    bool body = true;
    bool automatic = false;
    bool normal = false;
    bool viaDefault = false;
    bool columnType = false;
    QString dependentColumn;
    bool referencedColumns = false;
    char special = 0;
    for (const DependencyEdge *e : edges) {
        body &= e->type == 0;
        automatic |= e->type == 'a';
        normal |= e->type == 'n';
        if (e->type == 'e' || e->type == 'x' || e->type == 'P' || e->type == 'S')
            special = e->type;
        referencedColumns |= !e->referencedColumn.isEmpty();
        if (!e->dependentColumn.isEmpty()) {
            dependentColumn = e->dependentColumn;
            viaDefault |= e->viaDefault;
            columnType |= !e->viaDefault;
        }
    }
    if (body)
        return r.classId == PgProc ? tr("calls it in its body") : tr("names it in its body");
    if (special == 'e')
        return tr("member of the extension");
    if (special == 'x')
        return tr("needs the extension");
    if (special == 'P' || special == 'S')
        return tr("partition of it");
    if (r.classId == PgNamespace)
        return tr("in the schema");
    switch (d.classId) {
    case PgConstraint:
        if (d.kind == 'f' && normal)
            return tr("foreign key references it");
        return automatic ? tr("constraint on it") : tr("constraint uses it");
    case PgTrigger:
        if (r.classId == PgProc)
            return tr("trigger executes it");
        if (automatic)
            return tr("trigger on it");
        // Only the columns of UPDATE OF are a trigger's own.
        return referencedColumns ? tr("trigger fires on update of it") : tr("trigger uses it");
    case PgPolicy:
        return automatic ? tr("policy on it") : tr("policy uses it");
    case PgRewrite:
        return tr("rule uses it");
    case PgClass:
        if (d.kind == 'v')
            return tr("view's query uses it");
        if (d.kind == 'm')
            return tr("materialized view's query uses it");
        if (d.kind == 'i' || d.kind == 'I')
            return automatic ? tr("index on it") : tr("index uses it");
        if (d.kind == 'S' && automatic)
            return tr("sequence owned by it");
        if (viaDefault)
            return tr("default of column %1 uses it").arg(dependentColumn);
        if (columnType)
            return tr("column %1 is of this type").arg(dependentColumn);
        if (r.classId == PgClass && isTableKind(d.kind) && isTableKind(r.kind))
            return automatic ? tr("partition of it") : tr("inherits from it");
        if (r.classId == PgType)
            return tr("table of this type");
        break;
    case PgProc:
        if (d.kind == 'a')
            return tr("aggregate uses it");
        return r.classId == PgType ? tr("takes or returns it") : tr("body uses it");
    case PgType:
        if (d.kind == 'd' && r.classId == PgType)
            return tr("domain over it");
        if (d.kind == 'r' && r.classId == PgType)
            return tr("range over it");
        return tr("type uses it");
    default:
        break;
    }
    return tr("%1 uses it").arg(d.type);
}

// Whether a type change of the root's column affects what is at the other
// end of this edge.
bool touchesColumn(const DependencyGraph &graph, const DependencyEdge &e, const QString &column)
{
    if (e.referencedColumn == column || e.viaRowType)
        return true;
    if (e.type == 0)
        return e.bodyStar || e.bodyColumns.contains(column);
    // Partitions and inheriting tables have the column too.
    const DependencyObject &d = graph.objects[std::size_t(e.dependent)];
    const DependencyObject &r = graph.objects[std::size_t(e.referenced)];
    return e.referencedColumn.isEmpty() && e.dependentColumn.isEmpty() && d.classId == PgClass
        && r.classId == PgClass && isTableKind(d.kind) && isTableKind(r.kind)
        && (e.type == 'n' || e.type == 'a');
}

// For a body the parser gives up on: the tables in FROM, JOIN, INTO,
// UPDATE and USING, and whatever is called like a function. Good enough to
// know what it probably needs.
void collectTokens(const QByteArray &body, Collector &collector, BodyReferences &refs)
{
    const scope::Tokens tokens(body);
    for (const scope::Source &source : scope::tokenScope(tokens).sources) {
        if (!source.derived && !source.relation.isEmpty())
            collector.addRelation({source.schema, source.relation});
    }
    for (int i = 0; i + 1 < tokens.size(); ++i) {
        const sql::TokenKind kind = tokens.at(i).kind;
        const auto category
            = kind == sql::TokenKind::Keyword ? sql::keywordCategory(tokens.text(i)) : std::nullopt;
        // Keywords that may name a function, like notify, but not IN ( or VALUES (.
        if (kind != sql::TokenKind::Identifier && kind != sql::TokenKind::QuotedIdentifier
            && category != sql::KeywordCategory::Unreserved
            && category != sql::KeywordCategory::TypeFunctionName)
            continue;
        if (!tokens.isPunct(i + 1, '('))
            continue;
        QualifiedName name {QString(), tokens.name(i)};
        if (i >= 2 && tokens.isPunct(i - 1, '.') && scope::isName(tokens.at(i - 2).kind))
            name.schema = tokens.name(i - 2);
        if (name.schema != QLatin1String("pg_catalog")
            && std::ranges::find(refs.functions, name) == refs.functions.end())
            refs.functions.push_back(name);
    }
}

} // namespace

BodyReferences bodyReferences(const QString &language, const QByteArray &body,
                              const QByteArray &definition)
{
    BodyReferences refs;
    Collector collector(refs);
    if (language == QLatin1String("plpgsql")) {
        PgQueryPlpgsqlParseResult result = pg_query_parse_plpgsql(definition.constData());
        if (result.error) {
            refs.error = QString::fromUtf8(result.error->message);
        } else {
            refs.parsed = true;
            walkPlpgsql(QJsonDocument::fromJson(QByteArray(result.plpgsql_funcs)).array(),
                        collector, &refs.error, &refs.parsed);
        }
        pg_query_free_plpgsql_parse_result(result);
    } else {
        refs.parsed = collectSql(body, PG_QUERY_PARSE_DEFAULT, collector, &refs.error);
    }
    if (!refs.parsed)
        collectTokens(body, collector, refs);
    collector.finish();
    return refs;
}

QStringList splitSearchPath(const QString &setting, const QString &user)
{
    QStringList out;
    for (const QString &item : setting.split(QLatin1Char(','))) {
        const QString trimmed = item.trimmed();
        if (trimmed.isEmpty())
            continue;
        const QStringList parts = splitQualified(trimmed);
        const QString schema = parts.join(QLatin1Char('.'));
        out << (schema == QLatin1String("$user") ? user : schema);
    }
    return out;
}

std::optional<ObjectKind> objectKindOf(const DependencyObject &object)
{
    switch (object.classId) {
    case PgClass:
        if (!isRelationKind(object.kind) && object.kind != 'i' && object.kind != 'I')
            return std::nullopt;
        return object.kind == 'I' ? ObjectKind::Index : relationKind(object.kind);
    case PgProc:
        if (object.kind == 'p')
            return ObjectKind::Procedure;
        return object.kind == 'a' ? ObjectKind::Aggregate : ObjectKind::Function;
    case PgType:
        return object.kind == 'd' ? ObjectKind::Domain : ObjectKind::Type;
    case PgNamespace:
        return ObjectKind::Schema;
    case PgConstraint:
        return ObjectKind::Constraint;
    case PgTrigger:
        return ObjectKind::Trigger;
    case PgPolicy:
        return ObjectKind::Policy;
    case PgExtension:
        return ObjectKind::Extension;
    default:
        return std::nullopt;
    }
}

int DependencyGraph::find(Oid classId, Oid oid) const
{
    return index.value(keyOf(classId, oid), -1);
}

bool hasDependencies(ObjectKind kind)
{
    return catalogOf(kind).has_value();
}

std::vector<QByteArray> dependencyQueries(ObjectKind kind, Oid oid)
{
    if (!hasDependencies(kind))
        return {};
    std::vector<QByteArray> queries {EdgesQuery, ObjectsQuery, BodiesQuery, SearchPathQuery};
    if (hasColumns(kind))
        queries.push_back(QByteArray(ColumnsQuery).replace("%1", QByteArray::number(oid)));
    return queries;
}

DependencyGraph parseDependencies(ObjectKind kind, Oid oid, const std::vector<pg::Result> &results)
{
    DependencyGraph graph;
    if (results.size() < 4)
        return graph;
    const pg::Result &edges = results[0];
    const pg::Result &objects = results[1];
    const pg::Result &bodies = results[2];
    const pg::Result &searchPath = results[3];

    static const QSet<QString> systemSchemas {QStringLiteral("pg_catalog"),
                                              QStringLiteral("information_schema"),
                                              QStringLiteral("pg_toast")};
    for (int row = 0; row < objects.rowCount(); ++row) {
        DependencyObject o;
        o.classId = oidAt(objects, row, 0);
        o.oid = oidAt(objects, row, 1);
        o.type = text(objects, row, 2);
        o.schema = text(objects, row, 3);
        o.name = text(objects, row, 4);
        o.identity = text(objects, row, 5);
        const QByteArray kindChar = objects.value(row, 6);
        o.kind = kindChar.isEmpty() ? 0 : kindChar.front();
        o.system = o.oid < FirstNormalObjectId || systemSchemas.contains(o.schema);
        graph.index.insert(keyOf(o.classId, o.oid), int(graph.objects.size()));
        graph.objects.push_back(o);
    }

    struct RawEdge
    {
        int dependent;
        int referenced;
        Oid referencedClass;
        char type;
        bool viaDefault;
        QString dependentColumn;
        QString referencedColumn;
    };
    std::vector<RawEdge> raw;
    graph.owner.resize(graph.objects.size());
    for (std::size_t i = 0; i < graph.owner.size(); ++i)
        graph.owner[i] = int(i);
    for (int row = 0; row < edges.rowCount(); ++row) {
        const Oid refClass = oidAt(edges, row, 2);
        const int dependent = graph.find(oidAt(edges, row, 0), oidAt(edges, row, 1));
        const int referenced = graph.find(refClass, oidAt(edges, row, 3));
        if (dependent < 0 || referenced < 0)
            continue;
        // NOT NULL is a constraint of its own since PostgreSQL 18, but no
        // more than a property of the column for anyone looking.
        const DependencyObject &d = graph.objects[std::size_t(dependent)];
        if (d.classId == PgConstraint && d.kind == 'n')
            continue;
        const char type = edges.value(row, 4).front();
        // An internal dependency makes the dependent a part of what it
        // depends on, which is what anyone would see of it.
        // So is everything in pg_toast, the TOAST table's index included,
        // which depends on the table only through the TOAST table.
        if ((type == 'i' || d.schema == QLatin1String("pg_toast"))
            && graph.owner[std::size_t(dependent)] == dependent)
            graph.owner[std::size_t(dependent)] = referenced;
        raw.push_back({dependent, referenced, refClass, type, edges.value(row, 5) == "t",
                       text(edges, row, 6), text(edges, row, 7)});
    }

    QSet<quint64> bodyPairs;
    auto addEdge = [&](DependencyEdge edge) {
        if (edge.dependent == edge.referenced)
            return;
        // A body naming a table five times still depends on it once.
        if (edge.type == 0) {
            const quint64 pair = (quint64(edge.dependent) << 32) | uint(edge.referenced);
            if (bodyPairs.contains(pair))
                return;
            bodyPairs.insert(pair);
        }
        graph.edges.push_back(std::move(edge));
    };
    for (const RawEdge &e : raw) {
        DependencyEdge edge;
        edge.dependent = canonical(graph.owner, e.dependent);
        edge.referenced = canonical(graph.owner, e.referenced);
        edge.type = e.type;
        // A column of a part is no column of its owner.
        if (edge.dependent == e.dependent)
            edge.dependentColumn = e.dependentColumn;
        if (edge.referenced == e.referenced)
            edge.referencedColumn = e.referencedColumn;
        edge.viaDefault = e.viaDefault && !edge.dependentColumn.isEmpty();
        edge.viaRowType = e.referencedClass == PgType
            && graph.objects[std::size_t(edge.referenced)].classId == PgClass;
        addEdge(std::move(edge));
    }

    // What the bodies name, looked up the way the server would.
    QString user;
    QStringList sessionPath;
    for (int row = 0; row < searchPath.rowCount(); ++row) {
        user = text(searchPath, row, 0);
        sessionPath << text(searchPath, row, 1);
    }
    NameIndex relations;
    NameIndex functions;
    NameIndex types;
    for (std::size_t i = 0; i < graph.objects.size(); ++i) {
        const DependencyObject &o = graph.objects[i];
        if (o.system || o.name.isEmpty())
            continue;
        if (o.classId == PgClass && isRelationKind(o.kind))
            relations[o.name].push_back(int(i));
        else if (o.classId == PgProc)
            functions[o.name].push_back(int(i));
        else if (o.classId == PgType)
            types[o.name].push_back(int(i));
    }
    for (int row = 0; row < bodies.rowCount(); ++row) {
        const int function = graph.find(PgProc, oidAt(bodies, row, 0));
        if (function < 0)
            continue;
        const DependencyObject &self = graph.objects[std::size_t(function)];
        const BodyReferences refs
            = bodyReferences(text(bodies, row, 1), bodies.value(row, 2), bodies.value(row, 4));
        if (!refs.parsed)
            graph.unreadable << self.identity;
        const QStringList path
            = bodies.isNull(row, 3) ? sessionPath : splitSearchPath(text(bodies, row, 3), user);
        const int from = canonical(graph.owner, function);
        auto link = [&](const NameIndex &index, const std::vector<QualifiedName> &names,
                        bool rowColumns) {
            for (const QualifiedName &name : names) {
                for (int target : resolve(graph, index, name, path, self.schema)) {
                    DependencyEdge edge;
                    edge.dependent = from;
                    edge.referenced = canonical(graph.owner, target);
                    edge.viaRowType = !rowColumns
                        && graph.objects[std::size_t(edge.referenced)].classId == PgClass;
                    if (rowColumns) {
                        edge.bodyColumns = refs.columns;
                        edge.bodyStar = refs.star;
                    }
                    addEdge(std::move(edge));
                }
            }
        };
        link(relations, refs.relations, true);
        link(functions, refs.functions, false);
        link(types, refs.types, false);
    }

    graph.dependents.resize(graph.objects.size());
    for (std::size_t i = 0; i < graph.edges.size(); ++i)
        graph.dependents[std::size_t(graph.edges[i].referenced)].push_back(int(i));

    if (const auto classId = catalogOf(kind)) {
        const int root = graph.find(*classId, oid);
        graph.root = root < 0 ? -1 : canonical(graph.owner, root);
    }
    if (results.size() > 4) {
        for (int row = 0; row < results[4].rowCount(); ++row)
            graph.rootColumns << text(results[4], row, 0);
    }
    return graph;
}

Impact impactOf(const DependencyGraph &graph, const QString &column)
{
    Impact impact;
    if (graph.root < 0)
        return impact;
    std::vector<int> placed(graph.objects.size(), -1);
    placed[std::size_t(graph.root)] = -2;

    struct Step
    {
        int object;
        int dependent; // Its index in impact.dependents, -1 for the root.
    };
    // Twice: first what pg_depend says goes with the root, so that each of
    // those hangs under a parent it really goes with; then whatever only a
    // body names, and what in turn depends on that.
    for (const bool catalogOnly : {true, false}) {
        std::deque<Step> queue {{graph.root, -1}};
        if (!catalogOnly) {
            for (std::size_t i = 0; i < impact.dependents.size(); ++i)
                queue.push_back({impact.dependents[i].object, int(i)});
        }
        while (!queue.empty()) {
            const Step step = queue.front();
            queue.pop_front();
            std::map<int, std::vector<const DependencyEdge *>> groups;
            for (int e : graph.dependents[std::size_t(step.object)]) {
                const DependencyEdge &edge = graph.edges[std::size_t(e)];
                if (placed[std::size_t(edge.dependent)] != -1)
                    continue;
                if (catalogOnly && edge.type == 0)
                    continue;
                if (step.dependent < 0 && !column.isEmpty() && !touchesColumn(graph, edge, column))
                    continue;
                groups[edge.dependent].push_back(&edge);
            }
            std::vector<int> order;
            for (const auto &[object, edges] : groups)
                order.push_back(object);
            std::ranges::sort(order, [&](int a, int b) {
                const DependencyObject &x = graph.objects[std::size_t(a)];
                const DependencyObject &y = graph.objects[std::size_t(b)];
                return std::tie(x.type, x.identity) < std::tie(y.type, y.identity);
            });
            for (int object : order) {
                const std::vector<const DependencyEdge *> &edges = groups[object];
                Dependent d;
                d.object = object;
                d.parent = step.dependent;
                d.depth = step.dependent < 0
                    ? 1
                    : impact.dependents[std::size_t(step.dependent)].depth + 1;
                d.reason = reasonFor(graph, edges);
                for (const DependencyEdge *e : edges) {
                    if (!e->referencedColumn.isEmpty() && !d.columns.contains(e->referencedColumn))
                        d.columns << e->referencedColumn;
                }
                // Automatic beats normal, as in the server: a trigger on a
                // table that also names one of its columns goes with it.
                const auto has = [&](char type) {
                    return std::ranges::any_of(
                        edges, [type](const DependencyEdge *e) { return e->type == type; });
                };
                d.effect = !catalogOnly     ? DropEffect::FailsWhenRun
                    : has('n') && !has('a') ? DropEffect::Cascade
                                            : DropEffect::DroppedAlong;
                switch (d.effect) {
                case DropEffect::Cascade:
                    ++impact.cascade;
                    ++impact.dropped;
                    break;
                case DropEffect::DroppedAlong:
                    ++impact.dropped;
                    break;
                case DropEffect::FailsWhenRun:
                    ++impact.failing;
                    break;
                }
                placed[std::size_t(object)] = int(impact.dependents.size());
                impact.dependents.push_back(d);
                queue.push_back({object, placed[std::size_t(object)]});
            }
        }
    }
    return impact;
}

QString dropEffectName(DropEffect effect)
{
    switch (effect) {
    case DropEffect::Cascade:
        return tr("needs CASCADE, then dropped");
    case DropEffect::DroppedAlong:
        return tr("dropped with it");
    case DropEffect::FailsWhenRun:
        return tr("stays, fails when run");
    }
    return {};
}

QString impactSummary(const DependencyGraph &graph, const Impact &impact, bool column)
{
    // No translations are loaded to pluralise %n, so English picks its own.
    auto n = [](int count, const char *one, const char *many) {
        return (count == 1 ? tr(one) : tr(many)).arg(count);
    };
    QStringList out;
    if (impact.dependents.empty()) {
        out << (column ? tr("Nothing depends on this column.") : tr("Nothing depends on it."));
    } else if (column) {
        if (impact.dropped > 0)
            out << n(impact.dropped,
                     "%1 object depends on this column, directly or through another one.",
                     "%1 objects depend on this column, directly or through others.");
    } else if (impact.cascade > 0) {
        out << n(impact.cascade, "A plain DROP is refused: %1 object needs it.",
                 "A plain DROP is refused: %1 objects need it.")
            << n(impact.dropped, "DROP … CASCADE drops %1 object along with it.",
                 "DROP … CASCADE drops %1 objects along with it.");
    } else if (impact.dropped > 0) {
        out << n(impact.dropped, "DROP takes %1 object along with it without asking.",
                 "DROP takes %1 objects along with it without asking.");
    }
    if (impact.failing > 0) {
        out << n(impact.failing,
                 "%1 more only names it in a function body, which the server does not track: "
                 "it stays, and fails when it runs.",
                 "%1 more only name it in function bodies, which the server does not track: "
                 "they stay, and fail when they run.");
    }
    if (!graph.unreadable.isEmpty()) {
        QStringList names = graph.unreadable.mid(0, 5);
        if (graph.unreadable.size() > names.size())
            names << QStringLiteral("…");
        out << n(int(graph.unreadable.size()),
                 "%1 function body could not be parsed, so what it names was guessed from its "
                 "words: %2.",
                 "%1 function bodies could not be parsed, so what they name was guessed from "
                 "their words: %2.")
                   .arg(names.join(QStringLiteral(", ")));
    }
    return out.join(QLatin1Char(' '));
}

} // namespace slonisko::catalog
