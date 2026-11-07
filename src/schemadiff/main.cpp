// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Console.h"
#include "pg/QueryRunner.h"
#include "schemadiff/Diff.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <optional>

using namespace slonisko;
using docgen::err;

namespace {

// One line per change, as the script heads it, its hazards after a tab, so
// that grep and cut can take it apart.
QString changeList(const schemadiff::Diff &diff)
{
    QString out;
    for (const schemadiff::Change &change : diff.changes) {
        QString line = schemadiff::kindName(change.kind) + QLatin1Char(' ')
            + schemadiff::typeName(change.type) + QLatin1Char(' ') + change.name;
        if (!change.details.isEmpty())
            line += QStringLiteral(": ") + change.details;
        QStringList hazards;
        for (const schemadiff::Hazard &hazard : change.hazards) {
            if (!hazards.contains(schemadiff::hazardName(hazard.type)))
                hazards << schemadiff::hazardName(hazard.type);
        }
        if (!hazards.isEmpty())
            line += QLatin1Char('\t') + hazards.join(QStringLiteral(", "));
        out += line + QLatin1Char('\n');
    }
    return out;
}

QString summary(const schemadiff::Diff &diff)
{
    if (diff.isEmpty())
        return QStringLiteral("The schemas are the same.");
    int added = 0;
    int removed = 0;
    int hazardous = 0;
    for (const schemadiff::Change &change : diff.changes) {
        added += change.kind == schemadiff::ChangeKind::Added;
        removed += change.kind == schemadiff::ChangeKind::Removed;
        hazardous += !change.hazards.empty();
    }
    const int count = int(diff.changes.size());
    return QStringLiteral("%1 differences: %2 added, %3 removed, %4 changed; %5 with hazards.")
        .arg(count)
        .arg(added)
        .arg(removed)
        .arg(count - added - removed)
        .arg(hazardous);
}

bool writeText(const QString &path, const QString &text)
{
    if (path.isEmpty() || path == QLatin1String("-")) {
        QTextStream out(stdout);
        out << text << Qt::flush;
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text.toUtf8()) < 0) {
        err() << "slonisko-schemadiff: cannot write " << QDir::toNativeSeparators(path) << ": "
              << file.errorString() << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("slonisko-schemadiff"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Compares the schemas of two PostgreSQL databases and writes the SQL that makes the "
        "target like the reference. The script runs on the target; the reference is only "
        "read.\n\n"
        "Both connections are libpq connection strings or URIs, as psql takes them; for "
        "whatever they leave out, libpq's PG* environment variables and ~/.pgpass apply."));
    const QCommandLineOption help = parser.addHelpOption();
    const QCommandLineOption version = parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("target"),
                                 QStringLiteral("The database the script is for."),
                                 QStringLiteral("<target>"));
    parser.addPositionalArgument(QStringLiteral("reference"),
                                 QStringLiteral("The database whose schema to match."),
                                 QStringLiteral("<reference>"));

    const QCommandLineOption output({QStringLiteral("o"), QStringLiteral("output")},
                                    QStringLiteral("File to write the script, or with --check "
                                                   "the differences, into. Default: the "
                                                   "standard output."),
                                    QStringLiteral("file"));
    const QCommandLineOption schema({QStringLiteral("n"), QStringLiteral("schema")},
                                    QStringLiteral("Only this schema; * and ? are wildcards. "
                                                   "May be given more than once."),
                                    QStringLiteral("pattern"));
    const QCommandLineOption excludeSchema(
        {QStringLiteral("N"), QStringLiteral("exclude-schema")},
        QStringLiteral("Not this schema; may be given more than once."), QStringLiteral("pattern"));
    const QCommandLineOption password({QStringLiteral("W"), QStringLiteral("password")},
                                      QStringLiteral("Ask for the password first; it is used "
                                                     "for both databases."));
    const QCommandLineOption concurrently(QStringLiteral("concurrently"),
                                          QStringLiteral("Build and drop indexes on existing "
                                                         "tables CONCURRENTLY; the script then "
                                                         "has no transaction."));
    const QCommandLineOption noTransaction(QStringLiteral("no-transaction"),
                                           QStringLiteral("Leave out BEGIN and COMMIT."));
    const QCommandLineOption check(QStringLiteral("check"),
                                   QStringLiteral("Write the differences, one a line, instead "
                                                  "of the script, and exit with 3 when there "
                                                  "are any."));
    const QCommandLineOption exitCode(QStringLiteral("exit-code"),
                                      QStringLiteral("Exit with 3 when the schemas differ."));
    const QCommandLineOption quiet({QStringLiteral("q"), QStringLiteral("quiet")},
                                   QStringLiteral("Print nothing but errors."));
    parser.addOptions({output, schema, excludeSchema, password, concurrently, noTransaction, check,
                       exitCode, quiet});

    const bool parsed = parser.parse(QCoreApplication::arguments());
    if (parser.isSet(version))
        parser.showVersion();
    if (parser.isSet(help)) {
        QString text = parser.helpText();
        text = QStringLiteral("Usage: slonisko-schemadiff [options] <target> <reference>")
            + text.mid(text.indexOf(QLatin1Char('\n')));
        QTextStream(stdout) << text;
        return 0;
    }
    if (!parsed) {
        err() << "slonisko-schemadiff: " << parser.errorText() << "\n";
        return 2;
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 2) {
        err() << "slonisko-schemadiff: two connections, the target and the reference\n";
        return 2;
    }

    docgen::SchemaFilter filter;
    filter.include = parser.values(schema);
    filter.exclude = parser.values(excludeSchema);
    schemadiff::Options options;
    options.concurrentIndexes = parser.isSet(concurrently);
    const bool verbose = !parser.isSet(quiet);
    const bool failOnDrift = parser.isSet(check) || parser.isSet(exitCode);

    // libpq takes both from the environment whatever form the connections
    // are given in.
    if (parser.isSet(password))
        qputenv("PGPASSWORD", docgen::askPassword().toUtf8());
    if (qEnvironmentVariableIsEmpty("PGAPPNAME"))
        qputenv("PGAPPNAME", "slonisko-schemadiff");

    pg::Connection targetConnection;
    pg::QueryRunner targetRunner(&targetConnection);
    pg::Connection referenceConnection;
    pg::QueryRunner referenceRunner(&referenceConnection);
    targetConnection.open(positional.at(0).toUtf8());
    referenceConnection.open(positional.at(1).toUtf8());
    if (verbose)
        err() << "Reading both catalogs…\n" << Qt::flush;

    std::optional<docgen::Model> target;
    std::optional<docgen::Model> reference;
    int status = 0;
    bool failed = false;

    const auto finish = [&] {
        const schemadiff::Diff diff = schemadiff::compare(*target, *reference, options);
        QString text;
        if (parser.isSet(check)) {
            text = changeList(diff);
        } else {
            schemadiff::ScriptOptions scriptOptions;
            scriptOptions.targetName = target->database;
            scriptOptions.referenceName = reference->database;
            scriptOptions.transaction = !parser.isSet(noTransaction);
            text = schemadiff::script(diff, scriptOptions);
        }
        if (!writeText(parser.value(output), text))
            status = 1;
        else if (failOnDrift && !diff.isEmpty())
            status = 3;
        if (verbose)
            err() << summary(diff) << "\n" << Qt::flush;
        QCoreApplication::exit(status);
    };
    const auto loaded = [&](std::optional<docgen::Model> &slot, const char *side) {
        return [&, side](const docgen::Model &model, const QString &error) {
            if (failed)
                return;
            if (!error.isEmpty()) {
                err() << "slonisko-schemadiff: " << side << ": " << error.trimmed() << "\n";
                failed = true;
                status = 1;
                QCoreApplication::exit(status);
                return;
            }
            slot = model;
            if (target && reference)
                finish();
        };
    };
    schemadiff::loadModel(&targetRunner, filter, &app, loaded(target, "target"));
    schemadiff::loadModel(&referenceRunner, filter, &app, loaded(reference, "reference"));
    const int code = QCoreApplication::exec();
    return status != 0 ? status : code;
}
