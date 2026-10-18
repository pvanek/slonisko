// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Pages.h"

#include "catalog/ErdExport.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <map>
#include <type_traits>

namespace slonisko::docgen {

using catalog::ErdColumn;
using catalog::ErdEdge;
using catalog::ErdGraph;
using catalog::ErdNode;

Inline plain(const QString &text)
{
    return {Span {text, Span::Style::Plain, {}}};
}

Inline code(const QString &text)
{
    return {Span {text, Span::Style::Code, {}}};
}

Inline link(const QString &text, const Target &target, Span::Style style)
{
    return {Span {text, style, target}};
}

Block heading(const QString &text, int level, const QString &anchor)
{
    Block block;
    block.kind = Block::Kind::Heading;
    block.level = level;
    block.anchor = anchor;
    block.text = plain(text);
    return block;
}

Block paragraph(const Inline &text)
{
    Block block;
    block.kind = Block::Kind::Paragraph;
    block.text = text;
    return block;
}

Block codeBlock(const QString &sql)
{
    Block block;
    block.kind = Block::Kind::Code;
    block.code = sql;
    return block;
}

const Page *Site::page(const QString &id) const
{
    const auto it = std::ranges::find(pages, id, &Page::id);
    return it == pages.end() ? nullptr : &*it;
}

QString formatSize(qint64 bytes)
{
    static const char *const units[] = {"bytes", "kB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 bytes").arg(bytes);
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', value < 10 ? 1 : 0)
        .arg(QLatin1String(units[unit]));
}

namespace {

const QString HomePage = QStringLiteral("index");
const QString NamesPage = QStringLiteral("names");

// What a name becomes in a path or an anchor: ASCII letters, digits, _ and -,
// everything else an underscore.
QString slug(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const bool ascii = c.unicode() < 128 && (c.isLetterOrNumber() || c == QLatin1Char('-'));
        out += ascii ? c : QLatin1Char('_');
    }
    if (out.isEmpty() || !out.front().isLetter())
        out.prepend(QLatin1Char('x'));
    return out;
}

QString anchor(const char *prefix, const QString &name)
{
    return QLatin1String(prefix) + QLatin1Char('-') + slug(name);
}

// Hands out page ids, one per name, telling names apart that come out the
// same as a slug or differ only in case, which a file system on macOS or
// Windows would not tell apart either.
class Paths
{
public:
    QString claim(const QString &directory, const QString &name)
    {
        const QString base = directory + slug(name);
        QString id = base;
        for (int n = 2; m_used.contains(id.toLower()); ++n)
            id = base + QLatin1Char('-') + QString::number(n);
        m_used.insert(id.toLower());
        return id;
    }

private:
    QSet<QString> m_used;
};

QString formatRows(double rows)
{
    return rows < 0 ? QStringLiteral("not analysed") : QString::number(qint64(rows));
}

// The first paragraph of a comment, short enough for a list.
QString summary(const QString &comment)
{
    QString text
        = comment.section(QRegularExpression(QStringLiteral("\\n\\s*\\n")), 0, 0).simplified();
    if (text.size() > 160) {
        text.truncate(text.lastIndexOf(QLatin1Char(' '), 160));
        text += QStringLiteral("…");
    }
    return text;
}

void addText(std::vector<Block> &blocks, const QString &text)
{
    static const QRegularExpression paragraphs(QStringLiteral("\\n\\s*\\n"));
    for (const QString &part : text.split(paragraphs, Qt::SkipEmptyParts)) {
        const QString simplified = part.simplified();
        if (!simplified.isEmpty())
            blocks.push_back(paragraph(plain(simplified)));
    }
}

Block table(const QStringList &header)
{
    Block block;
    block.kind = Block::Kind::Table;
    for (const QString &title : header)
        block.header.push_back(plain(title));
    return block;
}

Block properties()
{
    Block block;
    block.kind = Block::Kind::Properties;
    return block;
}

void addProperty(Block &block, const QString &name, const Inline &value)
{
    if (!value.empty() && !(value.size() == 1 && value.front().text.isEmpty()))
        block.rows.push_back({plain(name), value});
}

void addProperty(Block &block, const QString &name, const QString &value)
{
    addProperty(block, name, plain(value));
}

QString relationKindName(const Relation &relation)
{
    if (relation.isPartition())
        return QStringLiteral("partition");
    switch (relation.kind) {
    case 'p':
        return QStringLiteral("partitioned table");
    case 'f':
        return QStringLiteral("foreign table");
    case 'v':
        return QStringLiteral("view");
    case 'm':
        return QStringLiteral("materialized view");
    default:
        return QStringLiteral("table");
    }
}

QString routineKindName(char kind)
{
    switch (kind) {
    case 'p':
        return QStringLiteral("procedure");
    case 'a':
        return QStringLiteral("aggregate");
    case 'w':
        return QStringLiteral("window function");
    default:
        return QStringLiteral("function");
    }
}

QString typeKindName(char kind)
{
    switch (kind) {
    case 'e':
        return QStringLiteral("enum");
    case 'c':
        return QStringLiteral("composite type");
    case 'd':
        return QStringLiteral("domain");
    case 'r':
        return QStringLiteral("range type");
    default:
        return QStringLiteral("type");
    }
}

QString constraintKindName(char type)
{
    switch (type) {
    case 'p':
        return QStringLiteral("primary key");
    case 'u':
        return QStringLiteral("unique");
    case 'f':
        return QStringLiteral("foreign key");
    case 'c':
        return QStringLiteral("check");
    case 'x':
        return QStringLiteral("exclusion");
    case 't':
        return QStringLiteral("constraint trigger");
    default:
        return QStringLiteral("constraint");
    }
}

ErdNode erdNode(const Relation &relation)
{
    ErdNode node;
    node.oid = relation.oid;
    node.schema = relation.schema;
    node.name = relation.name;
    QSet<QString> primary;
    QSet<QString> foreign;
    for (const Constraint &constraint : relation.constraints) {
        for (const QString &column : constraint.columns) {
            if (constraint.type == 'p')
                primary.insert(column);
            else if (constraint.type == 'f')
                foreign.insert(column);
        }
    }
    for (const Column &column : relation.columns) {
        const bool pk = primary.contains(column.name);
        const bool fk = foreign.contains(column.name);
        if (pk || fk)
            node.columns.push_back({column.name, column.type, pk, fk, column.notNull});
        else
            ++node.otherColumns;
    }
    return node;
}

bool inDiagrams(const Relation &relation)
{
    return relation.isTable() && !relation.isPartition();
}

// The foreign keys among the tables already in the graph.
void addEdges(const Model &model, ErdGraph &graph)
{
    QSet<Oid> involved;
    for (const ErdNode &node : graph.nodes)
        involved.insert(node.oid);
    for (const Relation &relation : model.relations) {
        if (!involved.contains(relation.oid))
            continue;
        for (const Constraint &constraint : relation.constraints) {
            if (constraint.type != 'f' || !involved.contains(constraint.referencedOid))
                continue;
            ErdEdge edge;
            edge.name = constraint.name;
            edge.from = relation.oid;
            edge.to = constraint.referencedOid;
            edge.fromColumns = constraint.columns;
            edge.toColumns = constraint.referencedColumns;
            edge.mandatory = std::ranges::all_of(constraint.columns, [&](const QString &name) {
                const Column *column = relation.column(name);
                return column && column->notNull;
            });
            graph.edges.push_back(std::move(edge));
        }
    }
}

class Builder
{
public:
    Builder(const Model &model, const PageOptions &options) : m_model(model), m_options(options) { }

    Site build();

private:
    void assignPages();
    Target relationTarget(Oid oid, const QString &anchor = {}) const;
    Target relationTarget(const QString &schema, const QString &name,
                          const QString &anchor = {}) const;
    Target routineTarget(const QString &qualifiedName) const;
    Inline relationLink(const Relation &relation, bool qualified = true) const;
    Inline columnLink(const QString &qualifiedColumn) const;
    std::vector<std::pair<QString, Target>> trail(const QString &schema = {}) const;
    Block diagram(const ErdGraph &graph) const;

    Page homePage() const;
    Page schemaPage(const Schema &schema) const;
    Page relationPage(const Relation &relation) const;
    Page routinePage(const Routine &routine) const;
    Page typePage(const UserType &type) const;
    Page sequencesPage(const QString &schema) const;
    Page namesPage() const;
    std::vector<NavItem> navigation() const;

    void addColumns(Page &page, const Relation &relation) const;
    void addConstraints(Page &page, const Relation &relation) const;
    void addReferences(Page &page, const Relation &relation) const;
    void addIndexes(Page &page, const Relation &relation) const;
    void addTriggers(Page &page, const Relation &relation) const;
    void addPolicies(Page &page, const Relation &relation) const;
    void addPartitions(Page &page, const Relation &relation) const;
    void addDependencies(Page &page, const Relation &relation) const;

    const Model &m_model;
    const PageOptions &m_options;
    QHash<QString, QString> m_schemaPages;
    QHash<QString, QString> m_sequencePages; // By schema.
    QHash<Oid, QString> m_relationPages;
    QHash<Oid, QString> m_routinePages;
    QHash<Oid, QString> m_typePages;
};

void Builder::assignPages()
{
    Paths paths;
    for (const QString &reserved : {HomePage, NamesPage})
        paths.claim({}, reserved);
    for (const Schema &schema : m_model.schemas) {
        const QString directory = paths.claim({}, schema.name) + QLatin1Char('/');
        m_schemaPages.insert(schema.name, directory + HomePage);
        m_sequencePages.insert(schema.name, directory + QStringLiteral("sequences"));
    }
    const auto directory = [this](const QString &schema, const char *kind) {
        const QString page = m_schemaPages.value(schema);
        return page.left(page.size() - HomePage.size()) + QLatin1String(kind) + QLatin1Char('/');
    };
    for (const Relation &relation : m_model.relations) {
        if (relation.isPartition() && !m_options.partitions)
            continue;
        m_relationPages.insert(
            relation.oid,
            paths.claim(directory(relation.schema, relation.isView() ? "views" : "tables"),
                        relation.name));
    }
    for (const Routine &routine : m_model.routines)
        m_routinePages.insert(routine.oid,
                              paths.claim(directory(routine.schema, "routines"), routine.name));
    for (const UserType &type : m_model.types)
        m_typePages.insert(type.oid, paths.claim(directory(type.schema, "types"), type.name));
}

Target Builder::relationTarget(Oid oid, const QString &anchorName) const
{
    if (const auto it = m_relationPages.constFind(oid); it != m_relationPages.cend())
        return {*it, anchorName};
    // A partition without a page of its own is listed on its parent's.
    if (const Relation *relation = m_model.relation(oid); relation && relation->isPartition())
        return relationTarget(relation->parentSchema, relation->parentName,
                              QStringLiteral("partitions"));
    return {};
}

Target Builder::relationTarget(const QString &schema, const QString &name,
                               const QString &anchorName) const
{
    const Relation *relation = m_model.relation(schema, name);
    return relation ? relationTarget(relation->oid, anchorName) : Target();
}

Target Builder::routineTarget(const QString &qualifiedName) const
{
    // A trigger names its function without arguments; a trigger function
    // takes none, so there is only one it can be.
    for (const Routine &routine : m_model.routines) {
        if (routine.qualifiedName() == qualifiedName && routine.arguments.isEmpty())
            return {m_routinePages.value(routine.oid), {}};
    }
    return {};
}

Inline Builder::relationLink(const Relation &relation, bool qualified) const
{
    return link(qualified ? relation.qualifiedName() : relation.name, relationTarget(relation.oid));
}

// schema.table.column, as a sequence's owner is given.
Inline Builder::columnLink(const QString &qualifiedColumn) const
{
    const QStringList parts = qualifiedColumn.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return plain(qualifiedColumn);
    return link(qualifiedColumn, relationTarget(parts[0], parts[1], anchor("col", parts[2])));
}

std::vector<std::pair<QString, Target>> Builder::trail(const QString &schema) const
{
    std::vector<std::pair<QString, Target>> out {{m_options.title, {HomePage, {}}}};
    if (!schema.isEmpty())
        out.push_back({schema, {m_schemaPages.value(schema), {}}});
    return out;
}

Block Builder::diagram(const ErdGraph &graph) const
{
    Block block;
    block.kind = Block::Kind::Diagram;
    block.dot = catalog::erdToDot(graph);
    block.mermaid = catalog::erdToMermaid(graph);
    for (const ErdNode &node : graph.nodes) {
        const Target target = relationTarget(node.oid);
        if (!target.isNull())
            block.nodeLinks.push_back({QStringLiteral("t%1").arg(node.oid), target});
    }
    return block;
}

Site Builder::build()
{
    assignPages();
    Site site;
    site.title = m_options.title;
    site.generator = m_options.generator;
    site.generatedAt = m_options.generatedAt;
    site.pages.push_back(homePage());
    for (const Schema &schema : m_model.schemas) {
        site.pages.push_back(schemaPage(schema));
        if (std::ranges::any_of(m_model.sequences,
                                [&](const Sequence &s) { return s.schema == schema.name; }))
            site.pages.push_back(sequencesPage(schema.name));
    }
    for (const Relation &relation : m_model.relations) {
        if (m_relationPages.contains(relation.oid))
            site.pages.push_back(relationPage(relation));
    }
    for (const Routine &routine : m_model.routines)
        site.pages.push_back(routinePage(routine));
    for (const UserType &type : m_model.types)
        site.pages.push_back(typePage(type));
    if (m_options.symbolIndex)
        site.pages.push_back(namesPage());
    site.navigation = navigation();

    // A schema without sequences has no page for them to link to.
    QSet<QString> ids;
    for (const Page &page : site.pages)
        ids.insert(page.id);
    for (Page &page : site.pages) {
        for (Block &block : page.blocks) {
            const auto prune = [&ids](Inline &text) {
                for (Span &span : text) {
                    if (!span.link.isNull() && !ids.contains(span.link.page))
                        span.link = {};
                }
            };
            prune(block.text);
            for (auto &row : block.rows)
                std::ranges::for_each(row, prune);
        }
    }
    return site;
}

Page Builder::homePage() const
{
    Page page;
    page.id = HomePage;
    page.title = m_options.title;
    page.kind = QStringLiteral("database");
    addText(page.blocks, m_options.description);
    addText(page.blocks, m_model.comment);

    Block about = properties();
    addProperty(about, QStringLiteral("Database"), m_model.database);
    addProperty(about, QStringLiteral("Server version"), m_model.serverVersion);
    addProperty(about, QStringLiteral("Owner"), m_model.owner);
    addProperty(about, QStringLiteral("Encoding"), m_model.encoding);
    addProperty(about, QStringLiteral("Generated"), m_options.generatedAt);
    page.blocks.push_back(about);

    page.blocks.push_back(heading(QStringLiteral("Schemas"), 2, QStringLiteral("schemas")));
    Block schemas
        = table({QStringLiteral("Schema"), QStringLiteral("Owner"), QStringLiteral("Tables"),
                 QStringLiteral("Views"), QStringLiteral("Routines"), QStringLiteral("Types"),
                 QStringLiteral("Comment")});
    for (const Schema &schema : m_model.schemas) {
        const auto count = [&schema](const auto &objects, auto predicate) {
            return QString::number(std::ranges::count_if(objects, [&](const auto &object) {
                return object.schema == schema.name && predicate(object);
            }));
        };
        schemas.rows.push_back(
            {link(schema.name, {m_schemaPages.value(schema.name), {}}), plain(schema.owner),
             plain(count(m_model.relations,
                         [](const Relation &r) { return r.isTable() && !r.isPartition(); })),
             plain(count(m_model.relations, [](const Relation &r) { return r.isView(); })),
             plain(count(m_model.routines, [](const Routine &) { return true; })),
             plain(count(m_model.types, [](const UserType &) { return true; })),
             plain(summary(schema.comment))});
    }
    page.blocks.push_back(schemas);

    if (!m_model.extensions.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Extensions"), 2, QStringLiteral("extensions")));
        Block extensions = table({QStringLiteral("Extension"), QStringLiteral("Version"),
                                  QStringLiteral("Schema"), QStringLiteral("Comment")});
        for (const Extension &extension : m_model.extensions)
            extensions.rows.push_back({plain(extension.name), plain(extension.version),
                                       plain(extension.schema), plain(extension.comment)});
        page.blocks.push_back(extensions);
    }

    if (m_options.symbolIndex) {
        page.blocks.push_back(heading(QStringLiteral("See also")));
        Block list;
        list.kind = Block::Kind::List;
        list.rows.push_back({link(QStringLiteral("Index of names"), {NamesPage, {}})});
        page.blocks.push_back(list);
    }
    return page;
}

Page Builder::schemaPage(const Schema &schema) const
{
    Page page;
    page.id = m_schemaPages.value(schema.name);
    page.title = schema.name;
    page.kind = QStringLiteral("schema");
    page.trail = trail();
    addText(page.blocks, schema.comment);
    Block about = properties();
    addProperty(about, QStringLiteral("Owner"), schema.owner);
    page.blocks.push_back(about);

    const auto relations = [&](auto predicate) {
        std::vector<const Relation *> out;
        for (const Relation &relation : m_model.relations) {
            if (relation.schema == schema.name && !relation.isPartition() && predicate(relation))
                out.push_back(&relation);
        }
        return out;
    };
    const auto addRelations = [&](const QString &title, const char *anchorName,
                                  const std::vector<const Relation *> &list, bool tables) {
        if (list.empty())
            return;
        page.blocks.push_back(heading(title, 2, QLatin1String(anchorName)));
        Block block = tables ? table({QStringLiteral("Name"), QStringLiteral("Rows"),
                                      QStringLiteral("Size"), QStringLiteral("Comment")})
                             : table({QStringLiteral("Name"), QStringLiteral("Comment")});
        for (const Relation *relation : list) {
            if (tables)
                block.rows.push_back(
                    {relationLink(*relation, false),
                     plain(relation->kind == 'f' ? QString() : formatRows(relation->rows)),
                     plain(relation->size > 0 ? formatSize(relation->size) : QString()),
                     plain(summary(relation->comment))});
            else
                block.rows.push_back(
                    {relationLink(*relation, false), plain(summary(relation->comment))});
        }
        page.blocks.push_back(block);
    };

    const auto tables = relations([](const Relation &r) { return r.kind == 'r' || r.kind == 'p'; });
    if (m_options.diagrams && !tables.empty() && int(tables.size()) <= catalog::ManyTables) {
        page.blocks.push_back(heading(QStringLiteral("Diagram"), 2, QStringLiteral("diagram")));
        page.blocks.push_back(diagram(schemaGraph(m_model, schema.name)));
    }
    addRelations(QStringLiteral("Tables"), "tables", tables, true);
    addRelations(QStringLiteral("Foreign tables"), "foreign-tables",
                 relations([](const Relation &r) { return r.kind == 'f'; }), false);
    addRelations(QStringLiteral("Views"), "views",
                 relations([](const Relation &r) { return r.kind == 'v'; }), false);
    addRelations(QStringLiteral("Materialized views"), "materialized-views",
                 relations([](const Relation &r) { return r.kind == 'm'; }), true);

    for (const char kind : {'f', 'p', 'a', 'w'}) {
        Block block = table({QStringLiteral("Name"), QStringLiteral("Returns"),
                             QStringLiteral("Language"), QStringLiteral("Comment")});
        for (const Routine &routine : m_model.routines) {
            if (routine.schema == schema.name && routine.kind == kind)
                block.rows.push_back(
                    {link(routine.signature(), {m_routinePages.value(routine.oid), {}}),
                     plain(routine.result), plain(routine.language),
                     plain(summary(routine.comment))});
        }
        if (block.rows.empty())
            continue;
        const QString title = routineKindName(kind);
        page.blocks.push_back(heading(title.front().toUpper() + title.mid(1) + QLatin1Char('s'), 2,
                                      slug(title) + QLatin1Char('s')));
        page.blocks.push_back(block);
    }

    Block types
        = table({QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("Comment")});
    for (const UserType &type : m_model.types) {
        if (type.schema == schema.name)
            types.rows.push_back({link(type.name, {m_typePages.value(type.oid), {}}),
                                  plain(typeKindName(type.kind)), plain(summary(type.comment))});
    }
    if (!types.rows.empty()) {
        page.blocks.push_back(heading(QStringLiteral("Types"), 2, QStringLiteral("types")));
        page.blocks.push_back(types);
    }

    Block sequences
        = table({QStringLiteral("Name"), QStringLiteral("Owned by"), QStringLiteral("Comment")});
    for (const Sequence &sequence : m_model.sequences) {
        if (sequence.schema == schema.name)
            sequences.rows.push_back(
                {link(sequence.name,
                      {m_sequencePages.value(schema.name), anchor("seq", sequence.name)}),
                 columnLink(sequence.ownedBy), plain(summary(sequence.comment))});
    }
    if (!sequences.rows.empty()) {
        page.blocks.push_back(heading(QStringLiteral("Sequences"), 2, QStringLiteral("sequences")));
        page.blocks.push_back(sequences);
    }
    return page;
}

Page Builder::relationPage(const Relation &relation) const
{
    Page page;
    page.id = m_relationPages.value(relation.oid);
    page.title = relation.qualifiedName();
    page.kind = relationKindName(relation);
    page.trail = trail(relation.schema);
    addText(page.blocks, relation.comment);

    Block about = properties();
    addProperty(about, QStringLiteral("Owner"), relation.owner);
    if (relation.kind == 'r' || relation.kind == 'm' || relation.kind == 'p')
        addProperty(about, QStringLiteral("Rows (estimated)"),
                    relation.kind == 'p' ? QString() : formatRows(relation.rows));
    if (relation.size > 0)
        addProperty(about, QStringLiteral("Size"), formatSize(relation.size));
    addProperty(about, QStringLiteral("Partition key"), relation.partitionKey);
    if (relation.isPartition()) {
        const Relation *parent = m_model.relation(relation.parentSchema, relation.parentName);
        addProperty(about, QStringLiteral("Partition of"),
                    parent ? relationLink(*parent)
                           : plain(relation.parentSchema + QLatin1Char('.') + relation.parentName));
        addProperty(about, QStringLiteral("Bound"), code(relation.partitionBound));
    }
    addProperty(about, QStringLiteral("Server"), relation.foreignServer);
    addProperty(about, QStringLiteral("Tablespace"), relation.tablespace);
    addProperty(about, QStringLiteral("Options"), relation.options);
    if (relation.unlogged)
        addProperty(about, QStringLiteral("Persistence"), QStringLiteral("unlogged"));
    if (relation.rowSecurity)
        addProperty(about, QStringLiteral("Row level security"), QStringLiteral("enabled"));
    page.blocks.push_back(about);

    addColumns(page, relation);
    addConstraints(page, relation);
    addReferences(page, relation);
    addIndexes(page, relation);
    addTriggers(page, relation);
    addPolicies(page, relation);
    addPartitions(page, relation);
    addDependencies(page, relation);

    if (m_options.diagrams && inDiagrams(relation)) {
        const ErdGraph graph = tableGraph(m_model, relation.oid);
        if (graph.nodes.size() > 1) {
            page.blocks.push_back(heading(QStringLiteral("Diagram"), 2, QStringLiteral("diagram")));
            page.blocks.push_back(diagram(graph));
        }
    }
    if (m_options.definitions) {
        page.blocks.push_back(
            heading(QStringLiteral("Definition"), 2, QStringLiteral("definition")));
        if (relation.isView()) {
            page.blocks.push_back(codeBlock(
                QStringLiteral("CREATE %1 %2 AS\n%3")
                    .arg(relation.kind == 'm' ? QStringLiteral("MATERIALIZED VIEW")
                                              : QStringLiteral("VIEW"),
                         quoteQualified(relation.schema, relation.name), relation.definition)));
        } else {
            page.blocks.push_back(codeBlock(tableDefinition(relation)));
        }
    }
    return page;
}

void Builder::addColumns(Page &page, const Relation &relation) const
{
    page.blocks.push_back(heading(QStringLiteral("Columns"), 2, QStringLiteral("columns")));
    const bool table = relation.isTable();
    Block block = table ? docgen::table({QStringLiteral("Column"), QStringLiteral("Type"),
                                         QStringLiteral("Not null"), QStringLiteral("Default"),
                                         QStringLiteral("Key"), QStringLiteral("Comment")})
                        : docgen::table({QStringLiteral("Column"), QStringLiteral("Type"),
                                         QStringLiteral("Comment")});
    const Constraint *primary = relation.primaryKey();
    for (const Column &column : relation.columns) {
        const bool inPrimary = primary && primary->columns.contains(column.name);
        Inline name = plain(column.name);
        if (inPrimary)
            name.front().style = Span::Style::Strong;
        if (!table) {
            block.rows.push_back({name, code(column.type), plain(column.comment)});
            block.rowAnchors << anchor("col", column.name);
            continue;
        }
        QString defaultValue = column.defaultValue;
        if (!column.identity.isEmpty())
            defaultValue = QStringLiteral("GENERATED %1 AS IDENTITY").arg(column.identity);
        else if (!column.generated.isEmpty())
            defaultValue = QStringLiteral("GENERATED ALWAYS AS (%1)").arg(column.generated);
        Inline key;
        if (inPrimary)
            key.push_back({QStringLiteral("PK"), Span::Style::Plain, {}});
        for (const Constraint &constraint : relation.constraints) {
            if (constraint.type != 'f' || !constraint.columns.contains(column.name))
                continue;
            const qsizetype at = constraint.columns.indexOf(column.name);
            if (!key.empty())
                key.push_back({QStringLiteral(", "), Span::Style::Plain, {}});
            key.push_back({QStringLiteral("FK → "), Span::Style::Plain, {}});
            const QString referenced = constraint.referencedColumns.value(at);
            key.push_back({constraint.referencedTable + QLatin1Char('.') + referenced,
                           Span::Style::Plain,
                           relationTarget(constraint.referencedOid, anchor("col", referenced))});
        }
        block.rows.push_back(
            {name, code(column.type), plain(column.notNull ? QStringLiteral("yes") : QString()),
             defaultValue.isEmpty() ? plain({}) : code(defaultValue), key, plain(column.comment)});
        block.rowAnchors << anchor("col", column.name);
    }
    page.blocks.push_back(block);
}

void Builder::addConstraints(Page &page, const Relation &relation) const
{
    Block foreign = table({QStringLiteral("Name"), QStringLiteral("Columns"),
                           QStringLiteral("References"), QStringLiteral("On update"),
                           QStringLiteral("On delete"), QStringLiteral("Comment")});
    Block others = table({QStringLiteral("Name"), QStringLiteral("Kind"),
                          QStringLiteral("Definition"), QStringLiteral("Comment")});
    for (const Constraint &constraint : relation.constraints) {
        if (constraint.type == 'f') {
            Inline references
                = link(constraint.referencedSchema + QLatin1Char('.') + constraint.referencedTable,
                       relationTarget(constraint.referencedOid));
            references.push_back({QStringLiteral(" (%1)").arg(
                                      constraint.referencedColumns.join(QLatin1String(", "))),
                                  Span::Style::Plain,
                                  {}});
            foreign.rows.push_back({plain(constraint.name),
                                    plain(constraint.columns.join(QLatin1String(", "))), references,
                                    plain(constraint.onUpdate), plain(constraint.onDelete),
                                    plain(constraint.comment)});
            foreign.rowAnchors << anchor("con", constraint.name);
        } else {
            others.rows.push_back({plain(constraint.name),
                                   plain(constraintKindName(constraint.type)),
                                   code(constraint.definition), plain(constraint.comment)});
            others.rowAnchors << anchor("con", constraint.name);
        }
    }
    if (!others.rows.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Constraints"), 2, QStringLiteral("constraints")));
        page.blocks.push_back(others);
    }
    if (!foreign.rows.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Foreign keys"), 2, QStringLiteral("foreign-keys")));
        page.blocks.push_back(foreign);
    }
}

void Builder::addReferences(Page &page, const Relation &relation) const
{
    Block block = table({QStringLiteral("Table"), QStringLiteral("Constraint"),
                         QStringLiteral("Columns"), QStringLiteral("Referenced columns")});
    for (const Relation &other : m_model.relations) {
        if (other.isPartition())
            continue;
        for (const Constraint &constraint : other.constraints) {
            if (constraint.type == 'f' && constraint.referencedOid == relation.oid)
                block.rows.push_back(
                    {relationLink(other),
                     link(constraint.name,
                          relationTarget(other.oid, anchor("con", constraint.name))),
                     plain(constraint.columns.join(QLatin1String(", "))),
                     plain(constraint.referencedColumns.join(QLatin1String(", ")))});
        }
    }
    if (block.rows.empty())
        return;
    page.blocks.push_back(
        heading(QStringLiteral("Referenced by"), 2, QStringLiteral("referenced-by")));
    page.blocks.push_back(block);
}

void Builder::addIndexes(Page &page, const Relation &relation) const
{
    if (relation.indexes.empty())
        return;
    page.blocks.push_back(heading(QStringLiteral("Indexes"), 2, QStringLiteral("indexes")));
    Block block = table({QStringLiteral("Name"), QStringLiteral("Columns"), QStringLiteral("Kind"),
                         QStringLiteral("Size"), QStringLiteral("Comment")});
    for (const Index &index : relation.indexes) {
        QStringList kind {index.method};
        if (index.primary)
            kind << QStringLiteral("primary key");
        else if (index.unique)
            kind << QStringLiteral("unique");
        if (index.partial)
            kind << QStringLiteral("partial");
        if (!index.valid)
            kind << QStringLiteral("INVALID");
        QString columns = index.columns.mid(0, index.keyColumns).join(QLatin1String(", "));
        if (index.keyColumns < index.columns.size())
            columns += QStringLiteral(" INCLUDE (%1)")
                           .arg(index.columns.mid(index.keyColumns).join(QLatin1String(", ")));
        block.rows.push_back(
            {plain(index.name), code(columns), plain(kind.join(QLatin1String(", "))),
             plain(index.size > 0 ? formatSize(index.size) : QString()), plain(index.comment)});
        block.rowAnchors << anchor("idx", index.name);
    }
    page.blocks.push_back(block);
}

void Builder::addTriggers(Page &page, const Relation &relation) const
{
    if (relation.triggers.empty())
        return;
    page.blocks.push_back(heading(QStringLiteral("Triggers"), 2, QStringLiteral("triggers")));
    Block block
        = table({QStringLiteral("Name"), QStringLiteral("Function"), QStringLiteral("Enabled"),
                 QStringLiteral("Definition"), QStringLiteral("Comment")});
    for (const Trigger &trigger : relation.triggers) {
        block.rows.push_back({plain(trigger.name),
                              link(trigger.function, routineTarget(trigger.function)),
                              plain(trigger.enabled ? QStringLiteral("yes") : QStringLiteral("no")),
                              code(trigger.definition), plain(trigger.comment)});
        block.rowAnchors << anchor("trg", trigger.name);
    }
    page.blocks.push_back(block);
}

void Builder::addPolicies(Page &page, const Relation &relation) const
{
    if (relation.policies.empty())
        return;
    page.blocks.push_back(heading(QStringLiteral("Policies"), 2, QStringLiteral("policies")));
    Block block
        = table({QStringLiteral("Name"), QStringLiteral("Command"), QStringLiteral("Kind"),
                 QStringLiteral("Roles"), QStringLiteral("Using"), QStringLiteral("With check")});
    for (const Policy &policy : relation.policies)
        block.rows.push_back({plain(policy.name), plain(policy.command),
                              plain(policy.permissive ? QStringLiteral("permissive")
                                                      : QStringLiteral("restrictive")),
                              plain(policy.roles.join(QLatin1String(", "))),
                              policy.using_.isEmpty() ? plain({}) : code(policy.using_),
                              policy.withCheck.isEmpty() ? plain({}) : code(policy.withCheck)});
    page.blocks.push_back(block);
}

void Builder::addPartitions(Page &page, const Relation &relation) const
{
    Block block = table({QStringLiteral("Partition"), QStringLiteral("Bound"),
                         QStringLiteral("Rows"), QStringLiteral("Size")});
    for (const Relation &child : m_model.relations) {
        if (child.parentSchema != relation.schema || child.parentName != relation.name)
            continue;
        const Target target
            = m_relationPages.contains(child.oid) ? relationTarget(child.oid) : Target();
        block.rows.push_back({link(child.qualifiedName(), target), code(child.partitionBound),
                              plain(child.kind == 'p' ? QString() : formatRows(child.rows)),
                              plain(child.size > 0 ? formatSize(child.size) : QString())});
    }
    if (block.rows.empty())
        return;
    page.blocks.push_back(heading(QStringLiteral("Partitions"), 2, QStringLiteral("partitions")));
    page.blocks.push_back(block);
}

void Builder::addDependencies(Page &page, const Relation &relation) const
{
    Block uses;
    uses.kind = Block::Kind::List;
    Block usedBy = uses;
    for (const Dependency &dependency : m_model.dependencies) {
        if (dependency.view == relation.oid) {
            if (const Relation *other = m_model.relation(dependency.relation))
                uses.rows.push_back({relationLink(*other)});
        } else if (dependency.relation == relation.oid) {
            if (const Relation *other = m_model.relation(dependency.view))
                usedBy.rows.push_back({relationLink(*other)});
        }
    }
    if (!uses.rows.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Reads from"), 2, QStringLiteral("reads-from")));
        page.blocks.push_back(uses);
    }
    if (!usedBy.rows.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Used by views"), 2, QStringLiteral("used-by")));
        page.blocks.push_back(usedBy);
    }
}

Page Builder::routinePage(const Routine &routine) const
{
    Page page;
    page.id = m_routinePages.value(routine.oid);
    page.title = routine.schema + QLatin1Char('.') + routine.signature();
    page.kind = routineKindName(routine.kind);
    page.trail = trail(routine.schema);
    addText(page.blocks, routine.comment);

    Block about = properties();
    addProperty(about, QStringLiteral("Arguments"), code(routine.arguments));
    addProperty(about, QStringLiteral("Returns"), code(routine.result));
    addProperty(about, QStringLiteral("Language"), routine.language);
    if (routine.kind != 'p')
        addProperty(about, QStringLiteral("Volatility"),
                    routine.volatility == 'i'       ? QStringLiteral("immutable")
                        : routine.volatility == 's' ? QStringLiteral("stable")
                                                    : QStringLiteral("volatile"));
    addProperty(about, QStringLiteral("Security"),
                routine.securityDefiner ? QStringLiteral("definer") : QStringLiteral("invoker"));
    addProperty(about, QStringLiteral("Owner"), routine.owner);
    page.blocks.push_back(about);

    Block triggers = table({QStringLiteral("Trigger"), QStringLiteral("On")});
    for (const Relation &relation : m_model.relations) {
        for (const Trigger &trigger : relation.triggers) {
            if (trigger.function == routine.qualifiedName() && routine.arguments.isEmpty())
                triggers.rows.push_back(
                    {link(trigger.name, relationTarget(relation.oid, anchor("trg", trigger.name))),
                     relationLink(relation)});
        }
    }
    if (!triggers.rows.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Used by triggers"), 2, QStringLiteral("triggers")));
        page.blocks.push_back(triggers);
    }
    if (m_options.definitions && !routine.definition.isEmpty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Definition"), 2, QStringLiteral("definition")));
        page.blocks.push_back(codeBlock(routine.definition));
    }
    return page;
}

Page Builder::typePage(const UserType &type) const
{
    Page page;
    page.id = m_typePages.value(type.oid);
    page.title = type.qualifiedName();
    page.kind = typeKindName(type.kind);
    page.trail = trail(type.schema);
    addText(page.blocks, type.comment);

    Block about = properties();
    addProperty(about, type.kind == 'r' ? QStringLiteral("Subtype") : QStringLiteral("Base type"),
                type.baseType.isEmpty() ? plain({}) : code(type.baseType));
    if (type.kind == 'd') {
        addProperty(about, QStringLiteral("Not null"),
                    type.notNull ? QStringLiteral("yes") : QStringLiteral("no"));
        addProperty(about, QStringLiteral("Default"),
                    type.defaultValue.isEmpty() ? plain({}) : code(type.defaultValue));
    }
    addProperty(about, QStringLiteral("Owner"), type.owner);
    page.blocks.push_back(about);

    if (!type.labels.isEmpty()) {
        page.blocks.push_back(heading(QStringLiteral("Values"), 2, QStringLiteral("values")));
        Block list;
        list.kind = Block::Kind::List;
        for (const QString &label : type.labels)
            list.rows.push_back({code(label)});
        page.blocks.push_back(list);
    }
    if (!type.attributes.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Attributes"), 2, QStringLiteral("attributes")));
        Block block = table(
            {QStringLiteral("Attribute"), QStringLiteral("Type"), QStringLiteral("Comment")});
        for (const Column &attribute : type.attributes)
            block.rows.push_back(
                {plain(attribute.name), code(attribute.type), plain(attribute.comment)});
        page.blocks.push_back(block);
    }
    if (!type.constraints.empty()) {
        page.blocks.push_back(
            heading(QStringLiteral("Constraints"), 2, QStringLiteral("constraints")));
        Block block = table(
            {QStringLiteral("Name"), QStringLiteral("Definition"), QStringLiteral("Comment")});
        for (const Constraint &constraint : type.constraints)
            block.rows.push_back(
                {plain(constraint.name), code(constraint.definition), plain(constraint.comment)});
        page.blocks.push_back(block);
    }
    return page;
}

Page Builder::sequencesPage(const QString &schema) const
{
    Page page;
    page.id = m_sequencePages.value(schema);
    page.title = schema + QStringLiteral(" sequences");
    page.kind = QStringLiteral("sequences");
    page.trail = trail(schema);
    Block block
        = table({QStringLiteral("Sequence"), QStringLiteral("Type"), QStringLiteral("Start"),
                 QStringLiteral("Increment"), QStringLiteral("Minimum"), QStringLiteral("Maximum"),
                 QStringLiteral("Cache"), QStringLiteral("Cycle"), QStringLiteral("Owned by"),
                 QStringLiteral("Comment")});
    for (const Sequence &sequence : m_model.sequences) {
        if (sequence.schema != schema)
            continue;
        block.rows.push_back(
            {plain(sequence.name), code(sequence.type), plain(QString::number(sequence.start)),
             plain(QString::number(sequence.increment)), plain(QString::number(sequence.minimum)),
             plain(QString::number(sequence.maximum)), plain(QString::number(sequence.cache)),
             plain(sequence.cycle ? QStringLiteral("yes") : QStringLiteral("no")),
             columnLink(sequence.ownedBy), plain(sequence.comment)});
        block.rowAnchors << anchor("seq", sequence.name);
    }
    page.blocks.push_back(block);
    return page;
}

Page Builder::namesPage() const
{
    struct Entry
    {
        QString name;
        QString kind;
        Inline where;
        Target target;
    };
    std::vector<Entry> entries;
    for (const Relation &relation : m_model.relations) {
        const Target page = relationTarget(relation.oid);
        if (!m_relationPages.contains(relation.oid))
            continue;
        const Inline in = link(relation.qualifiedName(), page);
        entries.push_back({relation.name, relationKindName(relation),
                           link(relation.schema, {m_schemaPages.value(relation.schema), {}}),
                           page});
        for (const Column &column : relation.columns)
            entries.push_back({column.name,
                               QStringLiteral("column"),
                               in,
                               {page.page, anchor("col", column.name)}});
        for (const Constraint &constraint : relation.constraints)
            entries.push_back({constraint.name,
                               constraintKindName(constraint.type),
                               in,
                               {page.page, anchor("con", constraint.name)}});
        for (const Index &index : relation.indexes)
            entries.push_back(
                {index.name, QStringLiteral("index"), in, {page.page, anchor("idx", index.name)}});
        for (const Trigger &trigger : relation.triggers)
            entries.push_back({trigger.name,
                               QStringLiteral("trigger"),
                               in,
                               {page.page, anchor("trg", trigger.name)}});
    }
    for (const Routine &routine : m_model.routines)
        entries.push_back({routine.signature(),
                           routineKindName(routine.kind),
                           link(routine.schema, {m_schemaPages.value(routine.schema), {}}),
                           {m_routinePages.value(routine.oid), {}}});
    for (const UserType &type : m_model.types)
        entries.push_back({type.name,
                           typeKindName(type.kind),
                           link(type.schema, {m_schemaPages.value(type.schema), {}}),
                           {m_typePages.value(type.oid), {}}});
    for (const Sequence &sequence : m_model.sequences)
        entries.push_back({sequence.name,
                           QStringLiteral("sequence"),
                           link(sequence.schema, {m_schemaPages.value(sequence.schema), {}}),
                           {m_sequencePages.value(sequence.schema), anchor("seq", sequence.name)}});
    std::ranges::stable_sort(entries, [](const Entry &a, const Entry &b) {
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.kind < b.kind;
    });

    Page page;
    page.id = NamesPage;
    page.title = QStringLiteral("Index of names");
    page.kind = QStringLiteral("index");
    page.trail = trail();

    // One section per initial, and a line of links to them on top.
    std::map<QString, Block> sections;
    for (const Entry &entry : entries) {
        const QChar first = entry.name.isEmpty() ? QLatin1Char('#') : entry.name.front().toUpper();
        const QString letter = first.isLetter() ? QString(first) : QStringLiteral("#");
        auto [it, added] = sections.try_emplace(letter);
        if (added)
            it->second
                = table({QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("In")});
        it->second.rows.push_back({link(entry.name, entry.target), plain(entry.kind), entry.where});
    }
    Inline letters;
    for (const auto &[letter, block] : sections) {
        if (!letters.empty())
            letters.push_back({QStringLiteral(" · "), Span::Style::Plain, {}});
        letters.push_back({letter, Span::Style::Strong, {NamesPage, anchor("letter", letter)}});
    }
    page.blocks.push_back(paragraph(letters));
    for (auto &[letter, block] : sections) {
        page.blocks.push_back(heading(letter, 2, anchor("letter", letter)));
        page.blocks.push_back(std::move(block));
    }
    return page;
}

std::vector<NavItem> Builder::navigation() const
{
    std::vector<NavItem> items;
    items.push_back({m_options.title, {HomePage, {}}, {}});
    for (const Schema &schema : m_model.schemas) {
        NavItem item {schema.name, {m_schemaPages.value(schema.name), {}}, {}};
        const auto group = [&](const QString &title, auto predicate,
                               const QHash<Oid, QString> &pages, const auto &objects) {
            NavItem node {title, {}, {}};
            for (const auto &object : objects) {
                if (object.schema == schema.name && predicate(object)
                    && pages.contains(object.oid)) {
                    QString name;
                    if constexpr (std::is_same_v<std::decay_t<decltype(object)>, Routine>)
                        name = object.signature();
                    else
                        name = object.name;
                    node.children.push_back({name, {pages.value(object.oid), {}}, {}});
                }
            }
            if (!node.children.empty())
                item.children.push_back(std::move(node));
        };
        group(
            QStringLiteral("Tables"), [](const Relation &r) { return r.isTable(); },
            m_relationPages, m_model.relations);
        group(
            QStringLiteral("Views"), [](const Relation &r) { return r.isView(); }, m_relationPages,
            m_model.relations);
        group(
            QStringLiteral("Routines"), [](const Routine &) { return true; }, m_routinePages,
            m_model.routines);
        group(
            QStringLiteral("Types"), [](const UserType &) { return true; }, m_typePages,
            m_model.types);
        if (std::ranges::any_of(m_model.sequences,
                                [&](const Sequence &s) { return s.schema == schema.name; }))
            item.children.push_back(
                {QStringLiteral("Sequences"), {m_sequencePages.value(schema.name), {}}, {}});
        items.push_back(std::move(item));
    }
    if (m_options.symbolIndex)
        items.push_back({QStringLiteral("Index of names"), {NamesPage, {}}, {}});
    return items;
}

} // namespace

Site buildSite(const Model &model, const PageOptions &options)
{
    PageOptions effective = options;
    if (effective.title.isEmpty())
        effective.title = model.database;
    return Builder(model, effective).build();
}

ErdGraph tableGraph(const Model &model, Oid table)
{
    ErdGraph graph;
    graph.focus = table;
    const Relation *focus = model.relation(table);
    if (!focus)
        return graph;
    QSet<Oid> involved {table};
    for (const Constraint &constraint : focus->constraints) {
        if (constraint.type == 'f')
            involved.insert(constraint.referencedOid);
    }
    for (const Relation &relation : model.relations) {
        if (!inDiagrams(relation))
            continue;
        for (const Constraint &constraint : relation.constraints) {
            if (constraint.type == 'f' && constraint.referencedOid == table)
                involved.insert(relation.oid);
        }
    }
    graph.nodes.push_back(erdNode(*focus));
    for (const Relation &relation : model.relations) {
        if (relation.oid != table && involved.contains(relation.oid) && inDiagrams(relation))
            graph.nodes.push_back(erdNode(relation));
    }
    addEdges(model, graph);
    return graph;
}

ErdGraph schemaGraph(const Model &model, const QString &schema)
{
    ErdGraph graph;
    QSet<Oid> involved;
    for (const Relation &relation : model.relations) {
        if (relation.schema != schema || !inDiagrams(relation))
            continue;
        involved.insert(relation.oid);
        for (const Constraint &constraint : relation.constraints) {
            if (constraint.type == 'f')
                involved.insert(constraint.referencedOid);
        }
    }
    for (const Relation &relation : model.relations) {
        if (involved.contains(relation.oid) && inDiagrams(relation))
            graph.nodes.push_back(erdNode(relation));
    }
    addEdges(model, graph);
    return graph;
}

QString tableDefinition(const Relation &relation)
{
    const QString name = quoteQualified(relation.schema, relation.name);
    QString sql;
    if (relation.isPartition()) {
        sql = QStringLiteral("CREATE TABLE %1 PARTITION OF %2\n    %3")
                  .arg(name, quoteQualified(relation.parentSchema, relation.parentName),
                       relation.partitionBound);
    } else {
        QStringList lines;
        for (const Column &column : relation.columns) {
            QString line
                = QStringLiteral("    %1 %2").arg(quoteIdentifier(column.name), column.type);
            if (!column.identity.isEmpty())
                line += QStringLiteral(" GENERATED %1 AS IDENTITY").arg(column.identity);
            else if (!column.generated.isEmpty())
                line += QStringLiteral(" GENERATED ALWAYS AS (%1) %2")
                            .arg(column.generated,
                                 column.virtualGenerated ? QStringLiteral("VIRTUAL")
                                                         : QStringLiteral("STORED"));
            else if (!column.defaultValue.isEmpty())
                line += QStringLiteral(" DEFAULT ") + column.defaultValue;
            if (column.notNull)
                line += QStringLiteral(" NOT NULL");
            lines << line;
        }
        for (const Constraint &constraint : relation.constraints)
            lines << QStringLiteral("    CONSTRAINT %1 %2")
                         .arg(quoteIdentifier(constraint.name), constraint.definition);
        sql = QStringLiteral("CREATE %1TABLE %2 (\n%3\n)")
                  .arg(relation.kind == 'f'    ? QStringLiteral("FOREIGN ")
                           : relation.unlogged ? QStringLiteral("UNLOGGED ")
                                               : QString(),
                       name, lines.join(QLatin1String(",\n")));
        if (!relation.partitionKey.isEmpty())
            sql += QStringLiteral("\nPARTITION BY ") + relation.partitionKey;
        if (!relation.foreignServer.isEmpty())
            sql += QStringLiteral("\nSERVER ") + quoteIdentifier(relation.foreignServer);
        if (!relation.options.isEmpty())
            sql += QStringLiteral("\nWITH (%1)").arg(relation.options);
    }
    sql += QLatin1Char(';');

    // The indexes behind constraints come with them.
    for (const Index &index : relation.indexes) {
        const bool fromConstraint
            = std::ranges::any_of(relation.constraints, [&](const Constraint &c) {
                  return c.name == index.name && (c.type == 'p' || c.type == 'u' || c.type == 'x');
              });
        if (!fromConstraint)
            sql += QLatin1Char('\n') + index.definition + QLatin1Char(';');
    }
    for (const Trigger &trigger : relation.triggers)
        sql += QLatin1Char('\n') + trigger.definition + QLatin1Char(';');
    if (relation.rowSecurity)
        sql += QStringLiteral("\nALTER TABLE %1 ENABLE ROW LEVEL SECURITY;").arg(name);
    const auto literal = [](const QString &text) {
        QString quoted = text;
        quoted.replace(QLatin1Char('\''), QLatin1String("''"));
        return QLatin1Char('\'') + quoted + QLatin1Char('\'');
    };
    if (!relation.comment.isEmpty())
        sql += QStringLiteral("\nCOMMENT ON TABLE %1 IS %2;").arg(name, literal(relation.comment));
    for (const Column &column : relation.columns) {
        if (!column.comment.isEmpty())
            sql += QStringLiteral("\nCOMMENT ON COLUMN %1.%2 IS %3;")
                       .arg(name, quoteIdentifier(column.name), literal(column.comment));
    }
    return sql;
}

} // namespace slonisko::docgen
