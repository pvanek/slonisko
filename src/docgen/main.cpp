// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Console.h"
#include "docgen/Generator.h"
#include "pg/QueryRunner.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

using namespace slonisko;
using docgen::err;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("slonisko-docgen"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));

    QStringList formats;
    for (const docgen::Format format : docgen::allFormats())
        formats << docgen::formatKey(format);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Writes documentation of a PostgreSQL database's schemas: tables, views, "
                       "routines, types and sequences, with their comments, diagrams and an "
                       "index of names. What is worth fixing in it, slonisko-quality-report "
                       "reports.\n\n"
                       "The format comes first: %1. \"slonisko-docgen <format> --help\" lists "
                       "the options of that format too.\n\n"
                       "The connection is a libpq connection string or URI, as psql takes it; "
                       "without one, libpq's PG* environment variables and ~/.pgpass apply.")
            .arg(formats.join(QLatin1String(", "))));
    const QCommandLineOption help = parser.addHelpOption();
    const QCommandLineOption version = parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("format"), formats.join(QLatin1String(", ")),
                                 QStringLiteral("<format>"));
    parser.addPositionalArgument(QStringLiteral("connection"),
                                 QStringLiteral("\"host=... dbname=...\" or postgresql://..."),
                                 QStringLiteral("[connection]"));

    const QCommandLineOption output({QStringLiteral("o"), QStringLiteral("output")},
                                    QStringLiteral("Directory to write into. Default: "
                                                   "<database>-schema."),
                                    QStringLiteral("dir"));
    const QCommandLineOption schema({QStringLiteral("n"), QStringLiteral("schema")},
                                    QStringLiteral("Only this schema; * and ? are wildcards. "
                                                   "May be given more than once."),
                                    QStringLiteral("pattern"));
    const QCommandLineOption excludeSchema(
        {QStringLiteral("N"), QStringLiteral("exclude-schema")},
        QStringLiteral("Not this schema; may be given more than once."), QStringLiteral("pattern"));
    const QCommandLineOption title({QStringLiteral("t"), QStringLiteral("title")},
                                   QStringLiteral("Title of the documentation. Default: the "
                                                  "database's name."),
                                   QStringLiteral("text"));
    const QCommandLineOption description(
        QStringLiteral("description"),
        QStringLiteral("Introduction for the home page, or @file to read it from a file."),
        QStringLiteral("text"));
    const QCommandLineOption password({QStringLiteral("W"), QStringLiteral("password")},
                                      QStringLiteral("Ask for the password first."));
    const QCommandLineOption noDefinitions(QStringLiteral("no-definitions"),
                                           QStringLiteral("Leave out the SQL definitions."));
    const QCommandLineOption noDiagrams(QStringLiteral("no-diagrams"),
                                        QStringLiteral("Leave out the diagrams."));
    const QCommandLineOption noIndex(QStringLiteral("no-index"),
                                     QStringLiteral("Leave out the index of names."));
    const QCommandLineOption partitions(QStringLiteral("partitions"),
                                        QStringLiteral("Give partitions pages of their own."));
    const QCommandLineOption quiet({QStringLiteral("q"), QStringLiteral("quiet")},
                                   QStringLiteral("Print nothing but errors and warnings."));
    parser.addOptions({output, schema, excludeSchema, title, description, password, noDefinitions,
                       noDiagrams, noIndex, partitions, quiet});

    // Options of one format only.
    const QCommandLineOption dot(QStringLiteral("dot"),
                                 QStringLiteral("Graphviz program drawing the diagrams. "
                                                "Default: dot from the PATH."),
                                 QStringLiteral("program"));
    const QCommandLineOption css(QStringLiteral("css"),
                                 QStringLiteral("Stylesheet instead of the built-in one."),
                                 QStringLiteral("file"));
    const QCommandLineOption antoraName(QStringLiteral("antora-name"),
                                        QStringLiteral("Name of the Antora component. Default: "
                                                       "schema."),
                                        QStringLiteral("name"), QStringLiteral("schema"));
    const QCommandLineOption antoraVersion(QStringLiteral("antora-version"),
                                           QStringLiteral("Version of the Antora component. "
                                                          "Default: none."),
                                           QStringLiteral("version"));

    // The format decides which options there are, so it is read first, and
    // the rest parsed once its options are known.
    const QStringList arguments = QCoreApplication::arguments();
    parser.parse(arguments);
    const QString formatArgument = parser.positionalArguments().value(0);
    const std::optional<docgen::Format> chosen = docgen::formatFromKey(formatArgument);
    if (chosen == docgen::Format::Html)
        parser.addOptions({dot, css});
    else if (chosen == docgen::Format::Antora)
        parser.addOptions({antoraName, antoraVersion});

    const bool parsed = parser.parse(arguments);
    if (parser.isSet(version))
        parser.showVersion();
    if (parser.isSet(help)) {
        // Qt puts [options] before every argument; the format comes first.
        QString text = parser.helpText();
        text = QStringLiteral("Usage: slonisko-docgen %1 [options] [connection]")
                   .arg(chosen ? formatArgument : QStringLiteral("<format>"))
            + text.mid(text.indexOf(QLatin1Char('\n')));
        QTextStream(stdout) << text;
        return 0;
    }
    if (!parsed) {
        err() << "slonisko-docgen: " << parser.errorText() << "\n";
        return 2;
    }
    const QStringList positional = parser.positionalArguments();
    if (formatArgument.isEmpty() || !chosen) {
        err() << "slonisko-docgen: "
              << (formatArgument.isEmpty() ? QStringLiteral("no format")
                                           : QStringLiteral("unknown format ") + formatArgument)
              << "; the first argument is one of " << formats.join(QLatin1String(", ")) << "\n";
        return 2;
    }
    if (positional.size() > 2) {
        err() << "slonisko-docgen: one connection at most\n";
        return 2;
    }

    docgen::Options options;
    options.schemas.include = parser.values(schema);
    options.schemas.exclude = parser.values(excludeSchema);
    options.pages.title = parser.value(title);
    if (parser.isSet(description)) {
        const std::optional<QString> text = docgen::textOrFile(parser.value(description));
        if (!text)
            return 2;
        options.pages.description = *text;
    }
    options.pages.definitions = !parser.isSet(noDefinitions);
    options.pages.diagrams = !parser.isSet(noDiagrams);
    options.pages.symbolIndex = !parser.isSet(noIndex);
    options.pages.partitions = parser.isSet(partitions);
    options.output.format = *chosen;
    if (*chosen == docgen::Format::Html) {
        options.output.dot = parser.value(dot);
        options.output.stylesheet = parser.value(css);
    } else if (*chosen == docgen::Format::Antora) {
        options.output.antoraName = parser.value(antoraName);
        options.output.antoraVersion = parser.value(antoraVersion);
    }
    const bool verbose = !parser.isSet(quiet);

    // libpq takes both from the environment whatever form the connection
    // is given in, a URI included.
    if (parser.isSet(password))
        qputenv("PGPASSWORD", docgen::askPassword().toUtf8());
    if (qEnvironmentVariableIsEmpty("PGAPPNAME"))
        qputenv("PGAPPNAME", "slonisko-docgen");

    pg::Connection connection;
    pg::QueryRunner runner(&connection);
    connection.open(positional.value(1).toUtf8());
    if (verbose)
        err() << "Reading the catalog…\n" << Qt::flush;

    int status = 0;
    docgen::loadModel(
        &runner, options.schemas, &app, [&](const docgen::Model &model, const QString &error) {
            if (!error.isEmpty()) {
                err() << "slonisko-docgen: " << error.trimmed() << "\n";
                status = 1;
                QCoreApplication::exit(status);
                return;
            }
            if (model.schemas.empty())
                err() << "slonisko-docgen: warning: no schema to document\n";
            if (!parser.isSet(output))
                options.output.directory = model.database + QStringLiteral("-schema");
            else
                options.output.directory = parser.value(output);
            if (verbose)
                err() << "Writing " << docgen::formatName(options.output.format) << " into "
                      << QDir::toNativeSeparators(options.output.directory) << "…\n"
                      << Qt::flush;
            const docgen::WriteResult result = docgen::generate(model, options);
            for (const QString &warning : result.warnings)
                err() << "slonisko-docgen: warning: " << warning << "\n";
            if (!result.ok()) {
                err() << "slonisko-docgen: " << result.error << "\n";
                status = 1;
            } else if (verbose) {
                err() << result.pages << " pages; start at "
                      << QDir::toNativeSeparators(result.entryPoint) << "\n";
            }
            QCoreApplication::exit(status);
        });
    const int code = QCoreApplication::exec();
    return status != 0 ? status : code;
}
