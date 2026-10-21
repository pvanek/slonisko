// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Import.h"
#include "pg/QueryRunner.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko::catalog;
using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;

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

class TestImport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_conninfo = serverConninfo();
        if (m_conninfo.isEmpty())
            QSKIP("No test server: set SLONISKO_TEST_CONNINFO or run through CTest.");
        QVERIFY(m_dir.isValid());
        m_connection = connect();
        QVERIFY(m_connection);
        QVERIFY(query("DROP SCHEMA IF EXISTS slonisko_import_test CASCADE;"
                      "CREATE SCHEMA slonisko_import_test;"
                      "CREATE TABLE slonisko_import_test.people ("
                      "    id int PRIMARY KEY, name text, note text,"
                      "    added date DEFAULT '2000-01-01')")
                    .ok());
    }

    void cleanupTestCase()
    {
        if (m_connection)
            query("DROP SCHEMA IF EXISTS slonisko_import_test CASCADE");
        delete m_connection;
    }

    void init()
    {
        if (m_connection)
            QVERIFY(query("TRUNCATE slonisko_import_test.people").ok());
    }

    // In batches, so the rows go in over several COPYs, and in the columns
    // the plan maps them to, whatever order the file has them in.
    void importsIntoATable()
    {
        ImportPlan plan = people(write("people.csv",
                                       "note,id,skipped,name\n"
                                       "first,1,x,Alice\n"
                                       ",2,x,\"Bo, \"\"the\"\" Builder\"\n"
                                       "\"\",3,x,\"two\nlines\"\n"
                                       "last,4,x,Dee\n"
                                       "\\back\\slash,5,x,Eve\n"));
        plan.columns = {{1, QStringLiteral("id"), {}},
                        {3, QStringLiteral("name"), {}},
                        {0, QStringLiteral("note"), {}}};
        plan.batchRows = 2;
        const auto [ok, message] = import(plan);
        QVERIFY2(ok, qPrintable(message));
        QVERIFY(message.contains(QLatin1String("5 rows")));
        const QueryOutcome rows = query("SELECT id, name, coalesce(note, '<null>'), added "
                                        "FROM slonisko_import_test.people ORDER BY id");
        QCOMPARE(rows.results.back().rowCount(), 5);
        const auto &r = rows.results.back();
        QCOMPARE(r.value(1, 1), QByteArray("Bo, \"the\" Builder"));
        QCOMPARE(r.value(1, 2), QByteArray("<null>")); // Unquoted and empty.
        QCOMPARE(r.value(2, 2), QByteArray("")); // Quoted and empty.
        QCOMPARE(r.value(2, 1), QByteArray("two\nlines"));
        QCOMPARE(r.value(4, 2), QByteArray("\\back\\slash"));
        QCOMPARE(r.value(0, 3), QByteArray("2000-01-01")); // Not in the file: the default.
    }

    // A bad row anywhere leaves the table as it was, emptied or not.
    void failsAsAWhole()
    {
        QVERIFY(
            query("INSERT INTO slonisko_import_test.people (id, name) VALUES (9, 'kept')").ok());
        ImportPlan plan = people(write("bad.csv", "id,name\n1,a\n2,b\n3,c\nfour,d\n5,e\n"));
        plan.columns = {{0, QStringLiteral("id"), {}}, {1, QStringLiteral("name"), {}}};
        plan.batchRows = 2;
        plan.truncate = true;
        const auto [ok, message] = import(plan);
        QVERIFY(!ok);
        // Counted over the batches, from the first row after the header.
        QVERIFY2(message.contains(QLatin1String("Row 4 ")), qPrintable(message));
        QVERIFY2(message.contains(QLatin1String("four")), qPrintable(message));
        QVERIFY(message.contains(QLatin1String("Nothing was imported")));
        QCOMPARE(query("SELECT name FROM slonisko_import_test.people").results.back().value(0, 0),
                 QByteArray("kept"));
        QCOMPARE(m_connection->transactionStatus(), PQTRANS_IDLE);
    }

    void createsTheTable()
    {
        ImportPlan plan;
        plan.path = write("new.csv", "id;when;amount\n1;2026-10-21;1,5\n2;2026-10-22;\n");
        plan.csv.delimiter = QLatin1Char(';');
        plan.schema = QStringLiteral("slonisko_import_test");
        plan.table = QStringLiteral("New Table");
        plan.create = true;
        plan.columns = {{0, QStringLiteral("id"), QStringLiteral("integer")},
                        {1, QStringLiteral("when"), QStringLiteral("date")},
                        {2, QStringLiteral("amount"), QStringLiteral("text")}};
        const auto [ok, message] = import(plan);
        QVERIFY2(ok, qPrintable(message));
        const QueryOutcome rows = query("SELECT pg_typeof(\"when\")::text, sum(id) OVER (), amount "
                                        "FROM slonisko_import_test.\"New Table\" ORDER BY id");
        QCOMPARE(rows.results.back().value(0, 0), QByteArray("date"));
        QCOMPARE(rows.results.back().value(0, 1), QByteArray("3"));
        QCOMPARE(rows.results.back().value(0, 2), QByteArray("1,5"));
        QVERIFY(rows.results.back().isNull(1, 2));

        // A failed import takes the table it made with it.
        plan.table = QStringLiteral("not made");
        plan.path = write("worse.csv", "id;when;amount\nx;y;z\n");
        QVERIFY(!import(plan).first);
        QCOMPARE(query("SELECT to_regclass('slonisko_import_test.\"not made\"') IS NULL")
                     .results.back()
                     .value(0, 0),
                 QByteArray("t"));
    }

    void cancels()
    {
        ImportPlan plan = people(write("many.csv", "id,name\n1,a\n2,b\n"));
        plan.columns = {{0, QStringLiteral("id"), {}}, {1, QStringLiteral("name"), {}}};
        ImportJob job(m_connection, plan);
        QSignalSpy finished(&job, &ImportJob::finished);
        job.start();
        job.cancel();
        QVERIFY(finished.wait(10'000));
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toString().contains(QLatin1String("stopped")));
        QCOMPARE(
            query("SELECT count(*) FROM slonisko_import_test.people").results.back().value(0, 0),
            QByteArray("0"));
    }

    void listsTheColumns()
    {
        QVERIFY(query("CREATE TABLE slonisko_import_test.\"It's\" ("
                      "    a int, dropped int, b numeric(5, 2),"
                      "    c int GENERATED ALWAYS AS (a * 2) STORED);"
                      "ALTER TABLE slonisko_import_test.\"It's\" DROP COLUMN dropped")
                    .ok());
        const QueryOutcome columns = query(
            tableColumnsQuery(QStringLiteral("slonisko_import_test"), QStringLiteral("It's")));
        QVERIFY(columns.ok());
        const auto &r = columns.results.back();
        QCOMPARE(r.rowCount(), 2);
        QCOMPARE(r.value(0, 0), QByteArray("a"));
        QCOMPARE(r.value(1, 0), QByteArray("b"));
        QCOMPARE(r.value(1, 1), QByteArray("numeric(5,2)"));
    }

    void reportsAMissingFile()
    {
        ImportPlan plan = people(m_dir.filePath(QStringLiteral("nowhere.csv")));
        plan.columns = {{0, QStringLiteral("id"), {}}};
        const auto [ok, message] = import(plan);
        QVERIFY(!ok);
        QVERIFY(message.contains(QLatin1String("nowhere.csv")));
    }

private:
    Connection *connect()
    {
        auto *connection = new Connection;
        QSignalSpy state(connection, &Connection::stateChanged);
        connection->open(m_conninfo);
        if (!QTest::qWaitFor(
                [&] {
                    return connection->state() == Connection::State::Ready
                        || connection->state() == Connection::State::Failed;
                },
                10'000)
            || connection->state() != Connection::State::Ready) {
            delete connection;
            return nullptr;
        }
        return connection;
    }

    QueryOutcome query(const QByteArray &sql)
    {
        QueryOutcome outcome;
        bool done = false;
        QMetaObject::Connection results = QObject::connect(
            m_connection, &Connection::resultReady, [&](const slonisko::pg::Result &r) {
                outcome.results.push_back(r);
                if (r.isError() && outcome.error.isEmpty())
                    outcome.error = r.errorMessage();
            });
        QMetaObject::Connection finished
            = QObject::connect(m_connection, &Connection::queryFinished, [&] { done = true; });
        if (!m_connection->execute(sql))
            outcome.error = QStringLiteral("not ready");
        else if (!QTest::qWaitFor([&] { return done; }, 10'000))
            outcome.error = QStringLiteral("timed out");
        QObject::disconnect(results);
        QObject::disconnect(finished);
        if (!outcome.ok())
            qWarning() << outcome.error;
        return outcome;
    }

    QString write(const char *name, const QByteArray &contents)
    {
        const QString path = m_dir.filePath(QString::fromUtf8(name));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(contents);
        return path;
    }

    ImportPlan people(const QString &path)
    {
        ImportPlan plan;
        plan.path = path;
        plan.schema = QStringLiteral("slonisko_import_test");
        plan.table = QStringLiteral("people");
        return plan;
    }

    std::pair<bool, QString> import(const ImportPlan &plan)
    {
        ImportJob job(m_connection, plan);
        QSignalSpy finished(&job, &ImportJob::finished);
        job.start();
        if (finished.isEmpty() && !finished.wait(20'000))
            return {false, QStringLiteral("timed out")};
        return {finished.first().at(0).toBool(), finished.first().at(1).toString()};
    }

    QByteArray m_conninfo;
    QTemporaryDir m_dir;
    Connection *m_connection = nullptr;
};

QTEST_MAIN(TestImport)
#include "tst_import.moc"
