// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Console.h"
#include "pg/QueryRunner.h"
#include "quality/Report.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

using namespace slonisko;
using docgen::err;

namespace {

std::optional<quality::Severity> severityFromKey(const QString &key)
{
    for (const quality::Severity s :
         {quality::Severity::Critical, quality::Severity::Warning, quality::Severity::Notice}) {
        if (quality::severityName(s).compare(key, Qt::CaseInsensitive) == 0)
            return s;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("slonisko-quality-report"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));

    QStringList formats;
    for (const docgen::Format format : docgen::allFormats())
        formats << docgen::formatKey(format);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral(
            "Writes a quality report of a PostgreSQL database: schema advice, "
            "missing, duplicate and unused indexes, broken dependencies, "
            "duplicates, and the health of the server and the database.\n\n"
            "The format comes first: %1. \"slonisko-quality-report <format> --help\" lists "
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
                                                   "<database>-quality-report."),
                                    QStringLiteral("dir"));
    const QCommandLineOption schema({QStringLiteral("n"), QStringLiteral("schema")},
                                    QStringLiteral("Only this schema; * and ? are wildcards. "
                                                   "May be given more than once."),
                                    QStringLiteral("pattern"));
    const QCommandLineOption excludeSchema(
        {QStringLiteral("N"), QStringLiteral("exclude-schema")},
        QStringLiteral("Not this schema; may be given more than once."), QStringLiteral("pattern"));
    const QCommandLineOption title({QStringLiteral("t"), QStringLiteral("title")},
                                   QStringLiteral("Title of the report. Default: \"<database> "
                                                  "quality report\"."),
                                   QStringLiteral("text"));
    const QCommandLineOption description(
        QStringLiteral("description"),
        QStringLiteral("Introduction for the summary page, or @file to read it from a file."),
        QStringLiteral("text"));
    const QCommandLineOption password({QStringLiteral("W"), QStringLiteral("password")},
                                      QStringLiteral("Ask for the password first."));
    const QCommandLineOption noHealth(QStringLiteral("no-health"),
                                      QStringLiteral("Read only the catalog: leave out the "
                                                     "statistics and the server's state."));
    const QCommandLineOption noNotices(QStringLiteral("no-notices"),
                                       QStringLiteral("Leave out what is only worth knowing."));
    const QCommandLineOption failOn(QStringLiteral("fail-on"),
                                    QStringLiteral("Exit with 3 when the report has a finding "
                                                   "this severe or more: critical, warning or "
                                                   "notice."),
                                    QStringLiteral("severity"));
    const QCommandLineOption quiet({QStringLiteral("q"), QStringLiteral("quiet")},
                                   QStringLiteral("Print nothing but errors and warnings."));
    parser.addOptions({output, schema, excludeSchema, title, description, password, noHealth,
                       noNotices, failOn, quiet});

    const QCommandLineOption css(QStringLiteral("css"),
                                 QStringLiteral("Stylesheet instead of the built-in one."),
                                 QStringLiteral("file"));
    const QCommandLineOption antoraName(QStringLiteral("antora-name"),
                                        QStringLiteral("Name of the Antora component. Default: "
                                                       "report."),
                                        QStringLiteral("name"), QStringLiteral("report"));
    const QCommandLineOption antoraVersion(QStringLiteral("antora-version"),
                                           QStringLiteral("Version of the Antora component. "
                                                          "Default: none."),
                                           QStringLiteral("version"));

    // The format decides which options there are, as for slonisko-docgen.
    const QStringList arguments = QCoreApplication::arguments();
    parser.parse(arguments);
    const QString formatArgument = parser.positionalArguments().value(0);
    const std::optional<docgen::Format> chosen = docgen::formatFromKey(formatArgument);
    if (chosen == docgen::Format::Html)
        parser.addOption(css);
    else if (chosen == docgen::Format::Antora)
        parser.addOptions({antoraName, antoraVersion});

    const bool parsed = parser.parse(arguments);
    if (parser.isSet(version))
        parser.showVersion();
    if (parser.isSet(help)) {
        QString text = parser.helpText();
        text = QStringLiteral("Usage: slonisko-quality-report %1 [options] [connection]")
                   .arg(chosen ? formatArgument : QStringLiteral("<format>"))
            + text.mid(text.indexOf(QLatin1Char('\n')));
        QTextStream(stdout) << text;
        return 0;
    }
    if (!parsed) {
        err() << "slonisko-quality-report: " << parser.errorText() << "\n";
        return 2;
    }
    const QStringList positional = parser.positionalArguments();
    if (formatArgument.isEmpty() || !chosen) {
        err() << "slonisko-quality-report: "
              << (formatArgument.isEmpty() ? QStringLiteral("no format")
                                           : QStringLiteral("unknown format ") + formatArgument)
              << "; the first argument is one of " << formats.join(QLatin1String(", ")) << "\n";
        return 2;
    }
    if (positional.size() > 2) {
        err() << "slonisko-quality-report: one connection at most\n";
        return 2;
    }
    std::optional<quality::Severity> failThreshold;
    if (parser.isSet(failOn)) {
        failThreshold = severityFromKey(parser.value(failOn));
        if (!failThreshold) {
            err() << "slonisko-quality-report: --fail-on takes critical, warning or notice\n";
            return 2;
        }
    }

    quality::Options options;
    options.schemas.include = parser.values(schema);
    options.schemas.exclude = parser.values(excludeSchema);
    options.report.title = parser.value(title);
    if (parser.isSet(description)) {
        const std::optional<QString> text = docgen::textOrFile(parser.value(description));
        if (!text)
            return 2;
        options.report.description = *text;
    }
    options.health = !parser.isSet(noHealth);
    options.notices = !parser.isSet(noNotices);
    options.output.format = *chosen;
    options.output.antoraName = QStringLiteral("report");
    if (*chosen == docgen::Format::Html) {
        options.output.stylesheet = parser.value(css);
    } else if (*chosen == docgen::Format::Antora) {
        options.output.antoraName = parser.value(antoraName);
        options.output.antoraVersion = parser.value(antoraVersion);
    }
    const bool verbose = !parser.isSet(quiet);

    if (parser.isSet(password))
        qputenv("PGPASSWORD", docgen::askPassword().toUtf8());
    if (qEnvironmentVariableIsEmpty("PGAPPNAME"))
        qputenv("PGAPPNAME", "slonisko-quality-report");

    pg::Connection connection;
    pg::QueryRunner runner(&connection);
    connection.open(positional.value(1).toUtf8());
    if (verbose)
        err() << "Reading the catalog" << (options.health ? " and statistics" : "") << "…\n"
              << Qt::flush;

    int status = 0;
    quality::loadReport(
        &runner, options.schemas, options.health, &app,
        [&](const quality::Loaded &loaded, const QString &error) {
            if (!error.isEmpty()) {
                err() << "slonisko-quality-report: " << error.trimmed() << "\n";
                status = 1;
                QCoreApplication::exit(status);
                return;
            }
            if (loaded.health) {
                for (const quality::Unavailable &u : loaded.health->unavailable)
                    err() << "slonisko-quality-report: warning: could not read the "
                          << quality::partTitle(u.part).toLower() << ": " << u.error << "\n";
            }
            options.output.directory = parser.isSet(output)
                ? parser.value(output)
                : loaded.model.database + QStringLiteral("-quality-report");
            if (verbose)
                err() << "Writing " << docgen::formatName(options.output.format) << " into "
                      << QDir::toNativeSeparators(options.output.directory) << "…\n"
                      << Qt::flush;
            const quality::Generated generated = quality::generate(loaded, options);
            const docgen::WriteResult &result = generated.written;
            for (const QString &warning : result.warnings)
                err() << "slonisko-quality-report: warning: " << warning << "\n";
            if (!result.ok()) {
                err() << "slonisko-quality-report: " << result.error << "\n";
                status = 1;
                QCoreApplication::exit(status);
                return;
            }
            if (verbose)
                err() << quality::count(generated.topics, quality::Severity::Critical)
                      << " critical, "
                      << quality::count(generated.topics, quality::Severity::Warning)
                      << " warnings, "
                      << quality::count(generated.topics, quality::Severity::Notice)
                      << " notices; start at " << QDir::toNativeSeparators(result.entryPoint)
                      << "\n";
            const std::optional<quality::Severity> worst = quality::worst(generated.topics);
            // Critical is the lowest value: "this severe or more" is <=.
            if (failThreshold && worst && int(*worst) <= int(*failThreshold))
                status = 3;
            QCoreApplication::exit(status);
        });
    const int code = QCoreApplication::exec();
    return status != 0 ? status : code;
}
