// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Generator.h"

#include "pg/QueryRunner.h"

#include <QDateTime>
#include <QStandardPaths>

#include <memory>

namespace slonisko::docgen {

void loadModel(pg::QueryRunner *runner, const SchemaFilter &filter, QObject *context,
               std::function<void(const Model &model, const QString &error)> done)
{
    const std::vector<QByteArray> queries = modelQueries(filter);
    auto results = std::make_shared<std::vector<pg::Result>>();
    auto error = std::make_shared<QString>();
    const std::size_t count = queries.size();
    for (const QByteArray &query : queries) {
        runner->run(query, context, [results, error, count, done](const pg::QueryOutcome &outcome) {
            if (!outcome.ok() && error->isEmpty())
                *error = outcome.error;
            results->push_back(outcome.results.empty() ? pg::Result() : outcome.results.back());
            if (results->size() < count)
                return;
            if (!error->isEmpty()) {
                done({}, *error);
                return;
            }
            done(parseModel(*results), {});
        });
    }
}

WriteResult generate(const Model &model, const Options &options)
{
    PageOptions pages = options.pages;
    if (pages.generator.isEmpty())
        pages.generator = generatorName();
    if (pages.generatedAt.isEmpty())
        pages.generatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);

    WriteOptions output = options.output;
    QStringList warnings;
    if (output.format == Format::Html && pages.diagrams && output.dot.isEmpty()) {
        output.dot = QStandardPaths::findExecutable(QStringLiteral("dot"));
        if (output.dot.isEmpty())
            warnings << QStringLiteral("Graphviz (dot) is not installed: the diagrams are left "
                                       "as their source.");
    }

    const Site site = buildSite(model, pages);
    WriteResult result = writeSite(site, output);
    result.warnings = warnings + result.warnings;
    return result;
}

QString generatorName()
{
    return QStringLiteral("slonisko-docgen " SLONISKO_VERSION);
}

} // namespace slonisko::docgen
