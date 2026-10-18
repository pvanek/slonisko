// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quality/Report.h"

#include "docgen/Generator.h"
#include "docgen/Pages.h"
#include "pg/QueryRunner.h"

#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

namespace slonisko::quality {

using docgen::Block;
using docgen::Inline;
using docgen::Page;
using docgen::Span;
using docgen::Target;

namespace {

const QString HomePage = QStringLiteral("index");
const QString StatementsPage = QStringLiteral("statements");
// How many issues the summary lists before sending to the sections.
const int FirstIssues = 50;

QString number(qint64 value)
{
    return QLocale(QLocale::English).toString(value);
}

Block table(const QStringList &header)
{
    Block block;
    block.kind = Block::Kind::Table;
    for (const QString &title : header)
        block.header.push_back(docgen::plain(title));
    return block;
}

Block properties()
{
    Block block;
    block.kind = Block::Kind::Properties;
    return block;
}

void addProperty(Block &block, const QString &name, const QString &value)
{
    if (!value.isEmpty())
        block.rows.push_back({docgen::plain(name), docgen::plain(value)});
}

void addText(std::vector<Block> &blocks, const QString &text)
{
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")))) {
        const QString simplified = part.simplified();
        if (!simplified.isEmpty())
            blocks.push_back(docgen::paragraph(docgen::plain(simplified)));
    }
}

Inline severity(Severity s)
{
    return {
        {severityName(s), s == Severity::Critical ? Span::Style::Strong : Span::Style::Plain, {}}};
}

Block emphasis(const QString &text)
{
    return docgen::paragraph({{text, Span::Style::Emphasis, {}}});
}

QString counted(int n, const char *one, const char *many)
{
    return n == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one))
                  : QStringLiteral("%1 %2").arg(n).arg(QLatin1String(many));
}

class Builder
{
public:
    Builder(const docgen::Model &model, const Health *health, const std::vector<Topic> &topics,
            const ReportOptions &options)
        : m_model(model), m_health(health), m_topics(topics), m_options(options)
    { }

    docgen::Site build() const;

private:
    std::vector<std::pair<QString, Target>> trail() const
    {
        return {{m_options.title, {HomePage, {}}}};
    }
    Target topicTarget(const Topic &topic) const { return {sectionKey(topic.section), topic.id}; }

    Page homePage() const;
    Page sectionPage(Section section) const;
    Page statementsPage() const;
    void addOverview(Page &page) const;

    const docgen::Model &m_model;
    const Health *m_health;
    const std::vector<Topic> &m_topics;
    const ReportOptions &m_options;
};

docgen::Site Builder::build() const
{
    docgen::Site site;
    site.title = m_options.title;
    site.generator = m_options.generator;
    site.generatedAt = m_options.generatedAt;
    site.pages.push_back(homePage());
    site.navigation.push_back({QStringLiteral("Summary"), {HomePage, {}}, {}});
    for (const Section section : allSections()) {
        site.pages.push_back(sectionPage(section));
        site.navigation.push_back({sectionTitle(section), {sectionKey(section), {}}, {}});
    }
    site.pages.push_back(statementsPage());
    site.navigation.push_back({QStringLiteral("Suggested statements"), {StatementsPage, {}}, {}});
    return site;
}

Page Builder::homePage() const
{
    Page page;
    page.id = HomePage;
    page.title = m_options.title;
    page.kind = QStringLiteral("quality report");
    addText(page.blocks, m_options.description);

    Block about = properties();
    addProperty(about, QStringLiteral("Database"), m_model.database);
    addProperty(about, QStringLiteral("Server version"),
                m_health && !m_health->version.isEmpty() ? m_health->version
                                                         : m_model.serverVersion);
    if (m_health && m_health->database.size > 0)
        addProperty(about, QStringLiteral("Size"), docgen::formatSize(m_health->database.size));
    QStringList schemas;
    for (const docgen::Schema &schema : m_model.schemas)
        schemas << schema.name;
    addProperty(about, QStringLiteral("Schemas"), schemas.join(QLatin1String(", ")));
    if (m_health)
        addProperty(about, QStringLiteral("Connected as"), m_health->user);
    addProperty(about, QStringLiteral("Generated"), m_options.generatedAt);
    page.blocks.push_back(about);

    page.blocks.push_back(docgen::heading(QStringLiteral("Summary"), 2, QStringLiteral("summary")));
    Block summary = table({QStringLiteral("Section"), QStringLiteral("Critical"),
                           QStringLiteral("Warnings"), QStringLiteral("Notices")});
    for (const Section section : allSections()) {
        const auto cell = [&](Severity s) {
            const int n = count(m_topics, section, s);
            Inline text = docgen::plain(n == 0 ? QStringLiteral("–") : QString::number(n));
            if (n > 0 && s == Severity::Critical)
                text.front().style = Span::Style::Strong;
            return text;
        };
        summary.rows.push_back({docgen::link(sectionTitle(section), {sectionKey(section), {}}),
                                cell(Severity::Critical), cell(Severity::Warning),
                                cell(Severity::Notice)});
    }
    page.blocks.push_back(summary);
    if (!m_health)
        page.blocks.push_back(emphasis(
            QStringLiteral("Only the catalog was read: the statistics and the server's state "
                           "were left out.")));

    page.blocks.push_back(
        docgen::heading(QStringLiteral("To look at first"), 2, QStringLiteral("first")));
    Block first = table({QStringLiteral("Severity"), QStringLiteral("Object"),
                         QStringLiteral("Finding"), QStringLiteral("Topic")});
    int left = 0;
    for (const Severity s : {Severity::Critical, Severity::Warning}) {
        for (const Topic &topic : m_topics) {
            for (const Issue &issue : topic.issues) {
                if (issue.severity != s)
                    continue;
                if (int(first.rows.size()) >= FirstIssues) {
                    ++left;
                    continue;
                }
                first.rows.push_back({severity(s), docgen::plain(issue.object),
                                      docgen::plain(issue.message),
                                      docgen::link(topic.title, topicTarget(topic))});
            }
        }
    }
    if (first.rows.empty()) {
        page.blocks.push_back(emphasis(QStringLiteral("Nothing critical, and no warnings.")));
    } else {
        page.blocks.push_back(first);
        if (left > 0)
            page.blocks.push_back(docgen::paragraph(
                docgen::plain(QStringLiteral("And %1 more, in their sections.").arg(left))));
    }
    return page;
}

void Builder::addOverview(Page &page) const
{
    const Health &h = *m_health;
    page.blocks.push_back(
        docgen::heading(QStringLiteral("Overview"), 2, QStringLiteral("overview")));
    Block about = properties();
    addProperty(about, QStringLiteral("Version"), h.version);
    if (!h.startedAt.isEmpty())
        addProperty(about, QStringLiteral("Running since"), h.startedAt);
    if (h.has(Part::Server)) {
        addProperty(about, QStringLiteral("Role"),
                    h.standby ? QStringLiteral("standby") : QStringLiteral("primary"));
        addProperty(about, QStringLiteral("Connections"),
                    QStringLiteral("%1 of %2").arg(h.connections).arg(h.maxConnections));
    }
    if (h.has(Part::Database)) {
        const DatabaseHealth &d = h.database;
        addProperty(about, QStringLiteral("Database size"), docgen::formatSize(d.size));
        addProperty(about, QStringLiteral("Statistics since"),
                    d.statsReset.isEmpty() ? QStringLiteral("the server started") : d.statsReset);
        if (d.blocksHit + d.blocksRead > 0)
            addProperty(about, QStringLiteral("Cache hit ratio"),
                        QStringLiteral("%1 %").arg(100.0 * double(d.blocksHit)
                                                       / double(d.blocksHit + d.blocksRead),
                                                   0, 'f', 2));
        addProperty(about, QStringLiteral("Transactions"),
                    QStringLiteral("%1 committed, %2 rolled back")
                        .arg(number(d.commits), number(d.rollbacks)));
        addProperty(about, QStringLiteral("Transaction ID age"), number(d.xidAge));
    }
    page.blocks.push_back(about);

    if (!h.settings.empty()) {
        page.blocks.push_back(
            docgen::heading(QStringLiteral("Settings"), 3, QStringLiteral("overview-settings")));
        Block settings
            = table({QStringLiteral("Setting"), QStringLiteral("Value"), QStringLiteral("Set by")});
        for (const Setting &s : h.settings)
            settings.rows.push_back(
                {docgen::code(s.name), docgen::plain(s.display()), docgen::plain(s.source)});
        page.blocks.push_back(settings);
    }

    if (!h.tables.empty()) {
        std::vector<const TableHealth *> largest;
        for (const TableHealth &t : h.tables)
            largest.push_back(&t);
        std::ranges::stable_sort(largest, std::greater<>(),
                                 [](const TableHealth *t) { return t->size + t->indexesSize; });
        if (largest.size() > 15)
            largest.resize(15);
        page.blocks.push_back(docgen::heading(QStringLiteral("Largest tables"), 3,
                                              QStringLiteral("overview-tables")));
        Block tables
            = table({QStringLiteral("Table"), QStringLiteral("Size"), QStringLiteral("Indexes"),
                     QStringLiteral("Rows"), QStringLiteral("Dead rows"),
                     QStringLiteral("Vacuumed"), QStringLiteral("Analyzed")});
        const auto never = [](const QString &when) {
            return docgen::plain(when.isEmpty() ? QStringLiteral("never") : when);
        };
        for (const TableHealth *t : largest)
            tables.rows.push_back({docgen::plain(t->schema + QLatin1Char('.') + t->name),
                                   docgen::plain(docgen::formatSize(t->size)),
                                   docgen::plain(docgen::formatSize(t->indexesSize)),
                                   docgen::plain(number(t->liveRows)),
                                   docgen::plain(number(t->deadRows)), never(t->lastVacuum),
                                   never(t->lastAnalyze)});
        page.blocks.push_back(tables);
    }
}

Page Builder::sectionPage(Section section) const
{
    Page page;
    page.id = sectionKey(section);
    page.title = sectionTitle(section);
    page.kind = QStringLiteral("quality report");
    page.trail = trail();
    page.blocks.push_back(docgen::paragraph(docgen::plain(sectionExplanation(section))));
    if (section == Section::Health) {
        if (!m_health) {
            page.blocks.push_back(
                emphasis(QStringLiteral("Left out: only the catalog was read for this report.")));
            return page;
        }
        addOverview(page);
    }

    Block contents;
    contents.kind = Block::Kind::List;
    std::vector<Block> details;
    for (const Topic &topic : m_topics) {
        if (topic.section != section)
            continue;
        Inline item = docgen::link(topic.title, topicTarget(topic));
        if (!topic.unavailable.isEmpty())
            item.push_back({QStringLiteral(": not looked at"), Span::Style::Emphasis, {}});
        else
            item.push_back({topic.issues.empty() ? QStringLiteral(": nothing found")
                                                 : QStringLiteral(": ")
                                    + counted(int(topic.issues.size()), "finding", "findings"),
                            Span::Style::Plain,
                            {}});
        contents.rows.push_back({item});

        details.push_back(docgen::heading(topic.title, 2, topic.id));
        details.push_back(docgen::paragraph(docgen::plain(topic.explanation)));
        if (!topic.unavailable.isEmpty())
            details.push_back(emphasis(QStringLiteral("Not looked at. ") + topic.unavailable));
        if (topic.issues.empty()) {
            if (topic.unavailable.isEmpty())
                details.push_back(emphasis(QStringLiteral("Nothing found.")));
            continue;
        }
        const bool fixes
            = std::ranges::any_of(topic.issues, [](const Issue &i) { return !i.fix.isEmpty(); });
        QStringList header {QStringLiteral("Severity"), QStringLiteral("Object"),
                            QStringLiteral("Finding")};
        if (fixes)
            header << QStringLiteral("Suggestion");
        Block block = table(header);
        for (const Issue &issue : topic.issues) {
            std::vector<Inline> row {severity(issue.severity), docgen::plain(issue.object),
                                     docgen::plain(issue.message)};
            if (fixes)
                row.push_back(issue.fix.isEmpty() ? docgen::plain({}) : docgen::code(issue.fix));
            block.rows.push_back(std::move(row));
        }
        details.push_back(std::move(block));
    }
    if (section == Section::Health)
        page.blocks.push_back(
            docgen::heading(QStringLiteral("Findings"), 2, QStringLiteral("findings")));
    page.blocks.push_back(std::move(contents));
    std::ranges::move(details, std::back_inserter(page.blocks));
    return page;
}

Page Builder::statementsPage() const
{
    Page page;
    page.id = StatementsPage;
    page.title = QStringLiteral("Suggested statements");
    page.kind = QStringLiteral("quality report");
    page.trail = trail();
    page.blocks.push_back(docgen::paragraph(docgen::plain(QStringLiteral(
        "What the findings suggest, together. Read each one before running it: a statement "
        "that drops an index, ends a session or rolls a transaction back cannot be undone, "
        "and only whoever knows the database can tell whether it is right."))));
    bool any = false;
    for (const Section section : allSections()) {
        QStringList script;
        for (const Topic &topic : m_topics) {
            if (topic.section != section || !topic.scripted)
                continue;
            QStringList fixes;
            for (const Issue &issue : topic.issues) {
                if (!issue.fix.isEmpty())
                    fixes << issue.fix;
            }
            if (fixes.isEmpty())
                continue;
            script << QStringLiteral("-- %1").arg(topic.title) << fixes << QString();
        }
        if (script.isEmpty())
            continue;
        any = true;
        script.removeLast();
        page.blocks.push_back(docgen::heading(sectionTitle(section), 2, sectionKey(section)));
        page.blocks.push_back(docgen::codeBlock(script.join(QLatin1Char('\n'))));
    }
    if (!any)
        page.blocks.push_back(emphasis(QStringLiteral("Nothing to suggest.")));
    return page;
}

} // namespace

docgen::Site buildReport(const docgen::Model &model, const Health *health,
                         const std::vector<Topic> &topics, const ReportOptions &options)
{
    ReportOptions effective = options;
    if (effective.title.isEmpty())
        effective.title = QStringLiteral("%1 quality report").arg(model.database);
    return Builder(model, health, topics, effective).build();
}

void loadReport(pg::QueryRunner *runner, const docgen::SchemaFilter &filter, bool health,
                QObject *context,
                std::function<void(const Loaded &loaded, const QString &error)> done)
{
    docgen::loadModel(
        runner, filter, context, [=](const docgen::Model &model, const QString &error) {
            if (!error.isEmpty() || !health) {
                done({model, std::nullopt}, error);
                return;
            }
            runner->run(versionQuery(), context, [=](const pg::QueryOutcome &outcome) {
                const int version = outcome.ok() && !outcome.results.empty()
                    ? parseVersion(outcome.results.back())
                    : 0;
                QString plpgsqlCheck;
                for (const docgen::Extension &extension : model.extensions) {
                    if (extension.name == QLatin1String("plpgsql_check"))
                        plpgsqlCheck = extension.schema;
                }
                const std::vector<HealthQuery> queries
                    = healthQueries(version, filter, plpgsqlCheck);
                // Each on its own, so that one the account may not run leaves
                // only its part out.
                auto results = std::make_shared<std::vector<pg::Result>>();
                auto errors = std::make_shared<QStringList>();
                for (const HealthQuery &query : queries) {
                    runner->run(query.sql, context, [=](const pg::QueryOutcome &o) {
                        results->push_back(o.results.empty() ? pg::Result() : o.results.back());
                        *errors << (o.ok() ? QString() : o.error);
                        if (results->size() < queries.size())
                            return;
                        done({model, parseHealth(version, queries, *results, *errors)}, {});
                    });
                }
            });
        });
}

Generated generate(const Loaded &loaded, const Options &options)
{
    ReportOptions report = options.report;
    if (report.generator.isEmpty())
        report.generator = generatorName();
    if (report.generatedAt.isEmpty())
        report.generatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);

    const Health *health = options.health && loaded.health ? &*loaded.health : nullptr;
    Generated generated;
    generated.topics = analyse(loaded.model, health, options.thresholds);
    if (!options.notices) {
        for (Topic &topic : generated.topics)
            std::erase_if(topic.issues,
                          [](const Issue &issue) { return issue.severity == Severity::Notice; });
    }
    const docgen::Site site = buildReport(loaded.model, health, generated.topics, report);
    generated.written = docgen::writeSite(site, options.output);
    return generated;
}

QString generatorName()
{
    return QStringLiteral("slonisko-quality-report " SLONISKO_VERSION);
}

} // namespace slonisko::quality
