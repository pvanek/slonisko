// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace slonisko::pg {

// pg_dump, pg_restore and psql, the server's own client programs, which
// back up and restore better than anything a client could write itself.
// They connect by the conninfo they are given; the password goes into their
// environment, where other users cannot read it as they could a command line.

enum class DumpFormat {
    Custom, // One compressed file pg_restore reads, choosing what to restore.
    Plain, // An SQL script, for psql or the editor.
    Directory, // A file per table, which can be dumped and restored in parallel.
    Tar,
};

enum class DumpContent { All, SchemaOnly, DataOnly };

struct DumpOptions
{
    DumpFormat format = DumpFormat::Custom;
    QString path; // A file, or a directory for DumpFormat::Directory.
    DumpContent content = DumpContent::All;
    // Only these; empty for all. Names as they are, not patterns.
    QStringList schemas;
    QStringList tables; // "schema.table", or "table" for any schema.
    bool clean = false; // Drop each object before creating it.
    bool create = false; // Create the database itself, and connect to it.
    bool noOwner = false;
    bool noPrivileges = false;
    bool inserts = false; // Data as INSERT statements rather than COPY.
    int jobs = 1; // Parallel, for DumpFormat::Directory only.
    int compression = -1; // 0 to 9; -1 for the format's default.
};

struct RestoreOptions
{
    QString path;
    DumpContent content = DumpContent::All;
    QStringList schemas;
    QStringList tables;
    bool clean = false;
    bool create = false;
    bool noOwner = false;
    bool noPrivileges = false;
    bool singleTransaction = false; // All or nothing.
    bool exitOnError = true; // Rather than carry on and report the errors at the end.
    int jobs = 1; // Not with singleTransaction.
};

// What a path holds, from its first bytes: a pg_dump archive pg_restore
// reads, an SQL script psql runs, or neither.
enum class ArchiveKind { Missing, Plain, Custom, Directory, Tar };
ArchiveKind archiveKind(const QString &path);
QString archiveKindName(ArchiveKind kind);
QString dumpFormatName(DumpFormat format);
QString dumpFormatSuffix(DumpFormat format); // "dump", "sql", "tar"; empty for a directory.

// A name as a pg_dump pattern that matches only that name: quoted, so that
// case and characters like * and . are taken as they are.
QString exactPattern(const QString &name);

QStringList dumpArguments(const DumpOptions &options, const QByteArray &conninfo);
QStringList restoreArguments(const RestoreOptions &options, const QByteArray &conninfo);
// A plain script runs in psql, which stops at the first error when told to.
QStringList psqlArguments(const RestoreOptions &options, const QByteArray &conninfo);
// The command as it could be typed into a shell, for the log.
QString commandLine(const QString &program, const QStringList &arguments);

struct ClientTool
{
    QString path;
    int version = 0; // As server_version_num has it: 180006 for 18.6.
};

// "pg_dump (PostgreSQL) 18.6" and the like, as --version writes it; 0 if
// it is not that.
int parseToolVersion(const QString &output);
QString versionText(int version); // 180006 as "18.6", 90624 as "9.6.24".

// Every copy of the program found in dirs, then on PATH, then where
// PostgreSQL's packages put the versions they install side by side.
std::vector<ClientTool> findClientTools(const QString &name, const QStringList &dirs = {});
// The copy to use with a server: the first one at least as new as the
// server, since pg_dump refuses to dump a newer one; if there is none, the
// newest. nullopt if tools is empty.
std::optional<ClientTool> chooseClientTool(const std::vector<ClientTool> &tools, int serverVersion);

// Runs one of the programs, passing on what it writes, line by line.
class ClientToolRun : public QObject
{
    Q_OBJECT

public:
    explicit ClientToolRun(QObject *parent = nullptr);
    ~ClientToolRun() override;

    void start(const QString &program, const QStringList &arguments, const QString &password);
    // Ends the program; finished() follows.
    void cancel();
    bool isRunning() const;

Q_SIGNALS:
    void output(const QString &line);
    void finished(bool ok, const QString &message);

private:
    void readOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);

    QProcess m_process;
    QString m_program;
    QByteArray m_partial; // Output after the last line break.
    bool m_cancelled = false;
};

} // namespace slonisko::pg
