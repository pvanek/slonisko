// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/ClientTools.h"
#include "pg/Connection.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko::pg;

namespace {

QByteArray serverConninfo()
{
    const QByteArray conninfo = qgetenv("SLONISKO_TEST_CONNINFO");
    if (!conninfo.isEmpty())
        return conninfo;
    QFile file(qEnvironmentVariable("SLONISKO_TEST_CONNINFO_FILE"));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll().trimmed();
}

} // namespace

class TestClientTools : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void dumpArgs()
    {
        DumpOptions options;
        options.path = QStringLiteral("/tmp/x.dump");
        options.schemas = {QStringLiteral("Sales")};
        options.tables = {QStringLiteral("public.my*table"), QStringLiteral("t")};
        options.content = DumpContent::SchemaOnly;
        options.noOwner = true;
        options.clean = true; // An archive leaves it to pg_restore.
        options.compression = 0;
        QCOMPARE(dumpArguments(options, "host=h dbname=d"),
                 (QStringList {
                     QStringLiteral("--verbose"), QStringLiteral("--format=custom"),
                     QStringLiteral("--file=/tmp/x.dump"), QStringLiteral("--schema-only"),
                     QStringLiteral("--schema=\"Sales\""),
                     QStringLiteral("--table=\"public\".\"my*table\""),
                     QStringLiteral("--table=\"t\""), QStringLiteral("--no-owner"),
                     QStringLiteral("--compress=0"), QStringLiteral("--dbname=host=h dbname=d")}));

        options = {};
        options.format = DumpFormat::Plain;
        options.path = QStringLiteral("out.sql");
        options.clean = true;
        options.create = true;
        options.inserts = true;
        options.jobs = 4; // Only for a directory.
        const QStringList plain = dumpArguments(options, "dbname=d");
        QVERIFY(plain.contains(QStringLiteral("--clean")));
        QVERIFY(plain.contains(QStringLiteral("--if-exists")));
        QVERIFY(plain.contains(QStringLiteral("--create")));
        QVERIFY(plain.contains(QStringLiteral("--inserts")));
        QVERIFY(!plain.join(QLatin1Char(' ')).contains(QLatin1String("--jobs")));

        options.format = DumpFormat::Directory;
        QVERIFY(dumpArguments(options, "dbname=d").contains(QStringLiteral("--jobs=4")));
    }

    void restoreArgs()
    {
        RestoreOptions options;
        options.path = QStringLiteral("x.dump");
        options.schemas = {QStringLiteral("s")};
        options.tables = {QStringLiteral("s.t")};
        options.clean = true;
        options.singleTransaction = true;
        options.jobs = 4; // Not in one transaction.
        QCOMPARE(
            restoreArguments(options, "dbname=d"),
            (QStringList {QStringLiteral("--verbose"), QStringLiteral("--schema=s"),
                          QStringLiteral("--table=t"), QStringLiteral("--clean"),
                          QStringLiteral("--if-exists"), QStringLiteral("--single-transaction"),
                          QStringLiteral("--exit-on-error"), QStringLiteral("--dbname=dbname=d"),
                          QStringLiteral("x.dump")}));
        QCOMPARE(
            psqlArguments(options, "dbname=d"),
            (QStringList {QStringLiteral("--no-psqlrc"), QStringLiteral("--echo-errors"),
                          QStringLiteral("--set=ON_ERROR_STOP=1"),
                          QStringLiteral("--single-transaction"), QStringLiteral("--file=x.dump"),
                          QStringLiteral("--dbname=dbname=d")}));
    }

    void commandLines()
    {
        QCOMPARE(commandLine(QStringLiteral("/usr/bin/pg_dump"),
                             {QStringLiteral("--file=a b.sql"), QStringLiteral("--schema=\"S\""),
                              QStringLiteral("--dbname=host=h port=5432")}),
                 QStringLiteral("/usr/bin/pg_dump '--file=a b.sql' '--schema=\"S\"' "
                                "'--dbname=host=h port=5432'"));
        QCOMPARE(
            commandLine(QStringLiteral("psql"), {QStringLiteral("--dbname=user='me' host='h'")}),
            QStringLiteral("psql \"--dbname=user='me' host='h'\""));
        QCOMPARE(commandLine(QStringLiteral("psql"), {QStringLiteral("it's $HOME")}),
                 QStringLiteral("psql 'it'\\''s $HOME'"));
    }

    void versions()
    {
        QCOMPARE(parseToolVersion(QStringLiteral("pg_dump (PostgreSQL) 18.6\n")), 180006);
        QCOMPARE(parseToolVersion(QStringLiteral("pg_restore (PostgreSQL) 17.2 (Debian 17.2-1)")),
                 170002);
        QCOMPARE(parseToolVersion(QStringLiteral("pg_dump (PostgreSQL) 19devel")), 190000);
        QCOMPARE(parseToolVersion(QStringLiteral("pg_dump (PostgreSQL) 9.6.24")), 90624);
        QCOMPARE(parseToolVersion(QStringLiteral("something else 1.0")), 0);
        QCOMPARE(versionText(180006), QStringLiteral("18.6"));
        QCOMPARE(versionText(90624), QStringLiteral("9.6.24"));
    }

    // The first that can dump the server, which must not be newer than it.
    void choosesATool()
    {
        const std::vector<ClientTool> tools {{QStringLiteral("/usr/bin/pg_dump"), 160004},
                                             {QStringLiteral("/v18/pg_dump"), 180001},
                                             {QStringLiteral("/v17/pg_dump"), 170005}};
        QCOMPARE(chooseClientTool(tools, 150002)->path, QStringLiteral("/usr/bin/pg_dump"));
        QCOMPARE(chooseClientTool(tools, 170000)->path, QStringLiteral("/v18/pg_dump"));
        // None new enough: the newest, which will say so itself.
        QCOMPARE(chooseClientTool(tools, 190000)->path, QStringLiteral("/v18/pg_dump"));
        QVERIFY(!chooseClientTool({}, 180000));
    }

    void archiveKinds()
    {
        auto write = [this](const char *name, const QByteArray &bytes) {
            const QString path = m_dir.filePath(QString::fromUtf8(name));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
                file.write(bytes);
            return path;
        };
        QCOMPARE(archiveKind(write("a.dump", "PGDMP\x01\x10")), ArchiveKind::Custom);
        QCOMPARE(archiveKind(write("a.sql", "--\n-- PostgreSQL database dump\n")),
                 ArchiveKind::Plain);
        QByteArray tar("toc.dat");
        tar.resize(512, '\0');
        tar.replace(257, 5, "ustar");
        QCOMPARE(archiveKind(write("a.tar", tar)), ArchiveKind::Tar);
        QCOMPARE(archiveKind(m_dir.filePath(QStringLiteral("none"))), ArchiveKind::Missing);
        QDir(m_dir.path()).mkdir(QStringLiteral("d"));
        QCOMPARE(archiveKind(m_dir.filePath(QStringLiteral("d"))), ArchiveKind::Missing);
        write("d/toc.dat", "PGDMP");
        QCOMPARE(archiveKind(m_dir.filePath(QStringLiteral("d"))), ArchiveKind::Directory);
    }

    void reportsAProgramThatIsNotThere()
    {
        ClientToolRun run;
        QSignalSpy finished(&run, &ClientToolRun::finished);
        run.start(m_dir.filePath(QStringLiteral("no_such_pg_dump")), {}, {});
        QVERIFY(finished.wait(5'000));
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toString().contains(QLatin1String("no_such_pg_dump")));
    }

    // A real backup and restore: pg_dump a schema, pg_restore it into
    // another database, and the same through a plain script and psql.
    void dumpsAndRestores()
    {
        const QByteArray conninfo = serverConninfo();
        if (conninfo.isEmpty())
            QSKIP("No test server: set SLONISKO_TEST_CONNINFO or run through CTest.");
        Connection admin;
        admin.open(conninfo);
        QTRY_COMPARE_WITH_TIMEOUT(admin.state(), Connection::State::Ready, 10'000);
        const int server = admin.serverVersion();
        const auto dump = chooseClientTool(findClientTools(QStringLiteral("pg_dump")), server);
        const auto restore
            = chooseClientTool(findClientTools(QStringLiteral("pg_restore")), server);
        const auto psql = chooseClientTool(findClientTools(QStringLiteral("psql")), server);
        if (!dump || !restore || !psql || dump->version / 10000 < server / 10000)
            QSKIP("No pg_dump, pg_restore and psql as new as the test server.");

        QVERIFY(exec(admin,
                     "DROP SCHEMA IF EXISTS \"Dump Me\" CASCADE;"
                     "CREATE SCHEMA \"Dump Me\";"
                     "CREATE TABLE \"Dump Me\".t (id int PRIMARY KEY, note text);"
                     "INSERT INTO \"Dump Me\".t VALUES (1, 'one'), (2, 'it''s');"
                     "CREATE TABLE public.slonisko_not_dumped (id int)"));
        QVERIFY(exec(admin, "DROP DATABASE IF EXISTS slonisko_restored"));
        QVERIFY(exec(admin, "CREATE DATABASE slonisko_restored"));

        DumpOptions options;
        options.path = m_dir.filePath(QStringLiteral("dump me.dump"));
        options.schemas = {QStringLiteral("Dump Me")};
        QString log;
        QVERIFY2(runTool(dump->path, dumpArguments(options, conninfo), &log), qPrintable(log));
        QVERIFY(log.contains(QLatin1String("dumping contents of table")));
        QCOMPARE(archiveKind(options.path), ArchiveKind::Custom);

        RestoreOptions into;
        into.path = options.path;
        into.singleTransaction = true;
        const QByteArray other = conninfo + " dbname=slonisko_restored";
        QVERIFY2(runTool(restore->path, restoreArguments(into, other), &log), qPrintable(log));

        Connection restored;
        restored.open(other);
        QTRY_COMPARE_WITH_TIMEOUT(restored.state(), Connection::State::Ready, 10'000);
        QVERIFY(exec(restored, "SELECT 1 FROM \"Dump Me\".t WHERE id = 2 AND note = 'it''s'"));
        QVERIFY(!exec(restored, "SELECT 1 FROM public.slonisko_not_dumped"));

        // A plain script, into the database it came from, after dropping
        // what it makes: psql runs it, and stops at the first error.
        DumpOptions script;
        script.format = DumpFormat::Plain;
        script.path = m_dir.filePath(QStringLiteral("dump.sql"));
        script.tables = {QStringLiteral("Dump Me.t")};
        script.clean = true;
        QVERIFY2(runTool(dump->path, dumpArguments(script, conninfo), &log), qPrintable(log));
        QCOMPARE(archiveKind(script.path), ArchiveKind::Plain);
        QVERIFY(exec(admin, "DELETE FROM \"Dump Me\".t WHERE id = 1"));
        RestoreOptions back;
        back.path = script.path;
        back.singleTransaction = true;
        QVERIFY2(runTool(psql->path, psqlArguments(back, conninfo), &log), qPrintable(log));
        QVERIFY(exec(admin, "SELECT 1 FROM \"Dump Me\".t WHERE id = 1"));

        // A failing one is reported as failing.
        QFile bad(m_dir.filePath(QStringLiteral("bad.sql")));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("SELECT 1;\nSELECT nonsense FROM nowhere;\nSELECT 2;\n");
        bad.close();
        back.path = bad.fileName();
        QVERIFY(!runTool(psql->path, psqlArguments(back, conninfo), &log));
        QVERIFY2(log.contains(QLatin1String("nowhere")), qPrintable(log));

        restored.close();
        QVERIFY(exec(admin, "DROP DATABASE slonisko_restored;"));
        QVERIFY(exec(admin,
                     "DROP SCHEMA \"Dump Me\" CASCADE;"
                     "DROP TABLE public.slonisko_not_dumped"));
    }

private:
    // True if sql ran without an error and returned a row, if it returns any.
    static bool exec(Connection &connection, const QByteArray &sql)
    {
        bool ok = true, done = false, rows = true;
        const auto results
            = QObject::connect(&connection, &Connection::resultReady, [&](const Result &r) {
                  ok = ok && !r.isError();
                  if (r.status() == PGRES_TUPLES_OK)
                      rows = r.rowCount() > 0;
              });
        const auto finished
            = QObject::connect(&connection, &Connection::queryFinished, [&] { done = true; });
        if (!connection.execute(sql) || !QTest::qWaitFor([&] { return done; }, 20'000))
            ok = false;
        QObject::disconnect(results);
        QObject::disconnect(finished);
        return ok && rows;
    }

    static bool runTool(const QString &program, const QStringList &arguments, QString *log)
    {
        ClientToolRun run;
        QSignalSpy finished(&run, &ClientToolRun::finished);
        log->clear();
        QObject::connect(&run, &ClientToolRun::output,
                         [log](const QString &line) { *log += line + QLatin1Char('\n'); });
        run.start(program, arguments, {});
        if (!finished.wait(60'000))
            return false;
        *log += finished.first().at(1).toString();
        return finished.first().at(0).toBool();
    }

    QTemporaryDir m_dir;
};

QTEST_MAIN(TestClientTools)
#include "tst_clienttools.moc"
