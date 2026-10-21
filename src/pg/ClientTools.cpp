// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/ClientTools.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace slonisko::pg {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("slonisko::pg", text);
}

QString conninfoArgument(const QByteArray &conninfo)
{
    return QStringLiteral("--dbname=") + QString::fromUtf8(conninfo);
}

// What both programs filter by, and how.
void addSelection(QStringList &args, DumpContent content, const QStringList &schemas,
                  const QStringList &tables, bool patterns)
{
    if (content == DumpContent::SchemaOnly)
        args << QStringLiteral("--schema-only");
    else if (content == DumpContent::DataOnly)
        args << QStringLiteral("--data-only");
    // pg_restore takes plain names, pg_dump patterns.
    for (const QString &schema : schemas)
        args << QStringLiteral("--schema=") + (patterns ? exactPattern(schema) : schema);
    for (const QString &table : tables) {
        if (patterns) {
            const qsizetype dot = table.indexOf(QLatin1Char('.'));
            args << QStringLiteral("--table=")
                    + (dot < 0 ? exactPattern(table)
                               : exactPattern(table.left(dot)) + QLatin1Char('.')
                               + exactPattern(table.mid(dot + 1)));
        } else {
            // pg_restore has no schema in --table; --schema says which.
            args << QStringLiteral("--table=") + table.section(QLatin1Char('.'), -1);
        }
    }
}

// Version-numbered directories, newest first: PostgreSQL's packages put
// several versions side by side, and only one of them is on PATH.
QStringList versionedDirs()
{
    QStringList dirs;
    for (int v = 30; v >= 9; --v) {
        const QString n = QString::number(v);
#if defined(Q_OS_WIN)
        dirs << QStringLiteral("C:/Program Files/PostgreSQL/%1/bin").arg(n);
#elif defined(Q_OS_MACOS)
        dirs << QStringLiteral("/Applications/Postgres.app/Contents/Versions/%1/bin").arg(n)
             << QStringLiteral("/opt/homebrew/opt/postgresql@%1/bin").arg(n)
             << QStringLiteral("/usr/local/opt/postgresql@%1/bin").arg(n);
#else
        dirs << QStringLiteral("/usr/lib/postgresql/%1/bin").arg(n) // Debian
             << QStringLiteral("/usr/lib/postgresql%1/bin").arg(n) // openSUSE
             << QStringLiteral("/usr/pgsql-%1/bin").arg(n); // Red Hat
#endif
    }
    return dirs;
}

int probeVersion(const QString &program)
{
    QProcess process;
    process.start(program, {QStringLiteral("--version")});
    if (!process.waitForFinished(5'000)) {
        process.kill();
        process.waitForFinished(1'000);
        return 0;
    }
    return parseToolVersion(QString::fromLocal8Bit(process.readAllStandardOutput()));
}

} // namespace

ArchiveKind archiveKind(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return ArchiveKind::Missing;
    if (info.isDir())
        return QFileInfo::exists(QDir(path).filePath(QStringLiteral("toc.dat")))
            ? ArchiveKind::Directory
            : ArchiveKind::Missing;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return ArchiveKind::Missing;
    const QByteArray head = file.read(512);
    if (head.startsWith("PGDMP"))
        return ArchiveKind::Custom;
    // A tar archive's header has "ustar" at 257, and pg_dump's first member
    // is its table of contents.
    if (head.size() >= 262 && head.mid(257, 5) == "ustar" && head.startsWith("toc.dat"))
        return ArchiveKind::Tar;
    return ArchiveKind::Plain;
}

QString archiveKindName(ArchiveKind kind)
{
    switch (kind) {
    case ArchiveKind::Missing:
        return tr("No such file or archive");
    case ArchiveKind::Plain:
        return tr("SQL script, run with psql");
    case ArchiveKind::Custom:
        return tr("Custom-format archive");
    case ArchiveKind::Directory:
        return tr("Directory-format archive");
    case ArchiveKind::Tar:
        return tr("Tar-format archive");
    }
    return {};
}

QString dumpFormatName(DumpFormat format)
{
    switch (format) {
    case DumpFormat::Custom:
        return tr("Custom archive");
    case DumpFormat::Plain:
        return tr("SQL script");
    case DumpFormat::Directory:
        return tr("Directory");
    case DumpFormat::Tar:
        return tr("Tar archive");
    }
    return {};
}

QString dumpFormatSuffix(DumpFormat format)
{
    switch (format) {
    case DumpFormat::Custom:
        return QStringLiteral("dump");
    case DumpFormat::Plain:
        return QStringLiteral("sql");
    case DumpFormat::Tar:
        return QStringLiteral("tar");
    case DumpFormat::Directory:
        break;
    }
    return {};
}

QString exactPattern(const QString &name)
{
    QString escaped = name;
    escaped.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QStringList dumpArguments(const DumpOptions &options, const QByteArray &conninfo)
{
    QStringList args {QStringLiteral("--verbose")};
    switch (options.format) {
    case DumpFormat::Custom:
        args << QStringLiteral("--format=custom");
        break;
    case DumpFormat::Plain:
        args << QStringLiteral("--format=plain");
        break;
    case DumpFormat::Directory:
        args << QStringLiteral("--format=directory");
        if (options.jobs > 1)
            args << QStringLiteral("--jobs=%1").arg(options.jobs);
        break;
    case DumpFormat::Tar:
        args << QStringLiteral("--format=tar");
        break;
    }
    args << QStringLiteral("--file=") + options.path;
    addSelection(args, options.content, options.schemas, options.tables, true);
    // In an archive these are pg_restore's to choose; in a script they are
    // written into it.
    if (options.format == DumpFormat::Plain) {
        if (options.clean)
            args << QStringLiteral("--clean") << QStringLiteral("--if-exists");
        if (options.create)
            args << QStringLiteral("--create");
    }
    if (options.noOwner)
        args << QStringLiteral("--no-owner");
    if (options.noPrivileges)
        args << QStringLiteral("--no-privileges");
    if (options.inserts)
        args << QStringLiteral("--inserts");
    if (options.compression >= 0 && options.format != DumpFormat::Tar)
        args << QStringLiteral("--compress=%1").arg(options.compression);
    args << conninfoArgument(conninfo);
    return args;
}

QStringList restoreArguments(const RestoreOptions &options, const QByteArray &conninfo)
{
    QStringList args {QStringLiteral("--verbose")};
    addSelection(args, options.content, options.schemas, options.tables, false);
    if (options.clean)
        args << QStringLiteral("--clean") << QStringLiteral("--if-exists");
    if (options.create)
        args << QStringLiteral("--create");
    if (options.noOwner)
        args << QStringLiteral("--no-owner");
    if (options.noPrivileges)
        args << QStringLiteral("--no-privileges");
    if (options.singleTransaction)
        args << QStringLiteral("--single-transaction");
    else if (options.jobs > 1)
        args << QStringLiteral("--jobs=%1").arg(options.jobs);
    if (options.exitOnError)
        args << QStringLiteral("--exit-on-error");
    args << conninfoArgument(conninfo) << options.path;
    return args;
}

QStringList psqlArguments(const RestoreOptions &options, const QByteArray &conninfo)
{
    // No ~/.psqlrc: it could change what the script does.
    QStringList args {QStringLiteral("--no-psqlrc"), QStringLiteral("--echo-errors")};
    if (options.exitOnError)
        args << QStringLiteral("--set=ON_ERROR_STOP=1");
    if (options.singleTransaction)
        args << QStringLiteral("--single-transaction");
    args << QStringLiteral("--file=") + options.path << conninfoArgument(conninfo);
    return args;
}

QString commandLine(const QString &program, const QStringList &arguments)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./:=@%+,-]+$"));
    static const QRegularExpression special(QStringLiteral("[\"$`\\\\!]"));
    QStringList parts;
    for (const QString &part : QStringList {program} + arguments) {
        if (plain.match(part).hasMatch()) {
            parts << part;
        } else if (!part.contains(QLatin1Char('\''))) {
            parts << QLatin1Char('\'') + part + QLatin1Char('\'');
        } else if (!special.match(part).hasMatch()) {
            // A conninfo quotes its values in single quotes; double ones
            // keep it readable.
            parts << QLatin1Char('"') + part + QLatin1Char('"');
        } else {
            QString quoted = part;
            quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
            parts << QLatin1Char('\'') + quoted + QLatin1Char('\'');
        }
    }
    return parts.join(QLatin1Char(' '));
}

int parseToolVersion(const QString &output)
{
    static const QRegularExpression version(
        QStringLiteral("\\(PostgreSQL\\)\\s+(\\d+)(?:\\.(\\d+))?(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = version.match(output);
    if (!m.hasMatch())
        return 0;
    const int major = m.captured(1).toInt();
    const int minor = m.captured(2).toInt();
    // Before 10 the major version had two parts: 9.6.24.
    if (major < 10)
        return major * 10000 + minor * 100 + m.captured(3).toInt();
    return major * 10000 + minor;
}

QString versionText(int version)
{
    if (version >= 100000)
        return QStringLiteral("%1.%2").arg(version / 10000).arg(version % 10000);
    return QStringLiteral("%1.%2.%3")
        .arg(version / 10000)
        .arg(version / 100 % 100)
        .arg(version % 100);
}

std::vector<ClientTool> findClientTools(const QString &name, const QStringList &dirs)
{
    QStringList candidates;
    for (const QString &dir : dirs) {
        const QString found = QStandardPaths::findExecutable(name, {dir});
        if (!found.isEmpty())
            candidates << found;
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty())
        candidates << onPath;
    for (const QString &dir : versionedDirs()) {
        if (!QFileInfo::exists(dir))
            continue;
        const QString found = QStandardPaths::findExecutable(name, {dir});
        if (!found.isEmpty())
            candidates << found;
    }

    std::vector<ClientTool> tools;
    QStringList seen; // The same program by another name, like a symlink.
    for (const QString &candidate : std::as_const(candidates)) {
        const QString real = QFileInfo(candidate).canonicalFilePath();
        if (seen.contains(real))
            continue;
        seen << real;
        if (const int version = probeVersion(candidate); version > 0)
            tools.push_back({candidate, version});
    }
    return tools;
}

std::optional<ClientTool> chooseClientTool(const std::vector<ClientTool> &tools, int serverVersion)
{
    if (tools.empty())
        return std::nullopt;
    const int serverMajor = serverVersion / 10000;
    for (const ClientTool &tool : tools) {
        if (tool.version / 10000 >= serverMajor)
            return tool;
    }
    return *std::ranges::max_element(tools, {}, &ClientTool::version);
}

// ClientToolRun.

ClientToolRun::ClientToolRun(QObject *parent) : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyRead, this, &ClientToolRun::readOutput);
    connect(&m_process, &QProcess::finished, this, &ClientToolRun::onFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            Q_EMIT finished(false,
                            tr("Could not run %1: %2").arg(m_program, m_process.errorString()));
    });
}

ClientToolRun::~ClientToolRun()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1'000);
    }
}

void ClientToolRun::start(const QString &program, const QStringList &arguments,
                          const QString &password)
{
    m_program = QFileInfo(program).fileName();
    m_partial.clear();
    m_cancelled = false;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!password.isEmpty())
        environment.insert(QStringLiteral("PGPASSWORD"), password);
    // Never a password prompt: there is no terminal to type it into.
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.setProcessEnvironment(environment);
    m_process.start(program, arguments);
}

void ClientToolRun::cancel()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_cancelled = true;
    m_process.terminate();
    // pg_dump ends at once; this is for one stuck on the network.
    QTimer::singleShot(5'000, this, [this] {
        if (m_process.state() != QProcess::NotRunning)
            m_process.kill();
    });
}

bool ClientToolRun::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void ClientToolRun::readOutput()
{
    m_partial += m_process.readAll();
    qsizetype end;
    while ((end = m_partial.indexOf('\n')) >= 0) {
        Q_EMIT output(QString::fromLocal8Bit(m_partial.left(end)).trimmed());
        m_partial.remove(0, end + 1);
    }
}

void ClientToolRun::onFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    if (!m_partial.isEmpty())
        Q_EMIT output(QString::fromLocal8Bit(std::exchange(m_partial, {})).trimmed());
    if (m_cancelled)
        Q_EMIT finished(false, tr("%1 was stopped.").arg(m_program));
    else if (status == QProcess::CrashExit)
        Q_EMIT finished(false, tr("%1 crashed.").arg(m_program));
    else if (exitCode != 0)
        Q_EMIT finished(false, tr("%1 failed with exit code %2.").arg(m_program).arg(exitCode));
    else
        Q_EMIT finished(true, tr("%1 finished.").arg(m_program));
}

} // namespace slonisko::pg
