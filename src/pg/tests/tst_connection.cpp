// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/Connection.h"
#include "sql/Splitter.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>

#include <memory>
#include <optional>
#include <vector>

using slonisko::pg::Connection;
using slonisko::pg::Result;
using State = Connection::State;

namespace {

// From SLONISKO_TEST_CONNINFO, or the file the CTest fixture writes after
// starting a server in Docker. Empty when there is no server.
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

#define REQUIRE_SERVER()                                                                           \
    const QByteArray conninfo = serverConninfo();                                                  \
    if (conninfo.isEmpty())                                                                        \
        QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");

bool waitForState(const Connection &c, State state, int timeoutMs = 10'000)
{
    return QTest::qWaitFor([&] { return c.state() == state; }, timeoutMs);
}

// Collects what a connection reports about its queries.
class Recorder
{
public:
    explicit Recorder(Connection &c)
    {
        QObject::connect(&c, &Connection::resultReady, &m_context,
                         [this](const Result &r) { results.push_back(r); });
        QObject::connect(&c, &Connection::queryFinished, &m_context, [this] { ++finished; });
    }

    std::vector<Result> results;
    int finished = 0;

private:
    QObject m_context;
};

// Runs sql to completion and returns its results.
std::vector<Result> run(Connection &c, const QByteArray &sql, int chunkSize = 0,
                        std::optional<QByteArray> copyData = std::nullopt)
{
    Recorder recorder(c);
    if (!c.execute(sql, chunkSize, std::move(copyData)))
        return {};
    (void)QTest::qWaitFor([&] { return recorder.finished > 0 || c.state() != State::Busy; },
                          30'000);
    return std::move(recorder.results);
}

// A port on which nothing listens.
quint16 closedPort()
{
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    return server.serverPort();
}

// Accepts connections and never says anything, like a server that hangs.
class SilentServer : public QTcpServer
{
public:
    SilentServer() { listen(QHostAddress::LocalHost); }

    QByteArray conninfo(const QByteArray &extra = {}) const
    {
        return "host=127.0.0.1 port=" + QByteArray::number(serverPort()) + " " + extra;
    }
};

} // namespace

class TestConnection : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Without a server.

    void invalidConninfo()
    {
        Connection c;
        QSignalSpy states(&c, &Connection::stateChanged);
        c.open("nonsense_option=1");

        QCOMPARE(c.state(), State::Failed);
        QVERIFY2(c.errorMessage().contains(QLatin1String("invalid connection option")),
                 qPrintable(c.errorMessage()));
        QCOMPARE(states.size(), 1);
    }

    void connectionRefused()
    {
        Connection c;
        c.open("host=127.0.0.1 port=" + QByteArray::number(closedPort()));
        QVERIFY(waitForState(c, State::Failed));
        QVERIFY2(c.errorMessage().contains(QLatin1String("refused")), qPrintable(c.errorMessage()));
    }

    void connectTimeout()
    {
        SilentServer server;
        Connection c;
        QElapsedTimer timer;
        timer.start();
        c.open(server.conninfo("connect_timeout=1"));
        QCOMPARE(c.state(), State::Connecting);

        QVERIFY(waitForState(c, State::Failed, 5'000));
        QVERIFY(timer.elapsed() >= 900);
        QCOMPARE(c.errorMessage(), QStringLiteral("Timed out connecting to the server"));
    }

    void connectTimeoutFromEnvironment()
    {
        SilentServer server;
        qputenv("PGCONNECT_TIMEOUT", "1");
        Connection c;
        c.open(server.conninfo());
        qunsetenv("PGCONNECT_TIMEOUT");

        QVERIFY(waitForState(c, State::Failed, 5'000));
        QCOMPARE(c.errorMessage(), QStringLiteral("Timed out connecting to the server"));
    }

    void closeWhileConnecting()
    {
        SilentServer server;
        Connection c;
        c.open(server.conninfo("connect_timeout=1"));
        QCOMPARE(c.state(), State::Connecting);

        QSignalSpy states(&c, &Connection::stateChanged);
        c.close();
        QCOMPARE(c.state(), State::Disconnected);
        QVERIFY(c.errorMessage().isEmpty());
        // The connect timeout must not fire after close().
        QTest::qWait(1'500);
        QCOMPARE(states.size(), 1);
    }

    void nothingToDoWhenDisconnected()
    {
        Connection c;
        QVERIFY(!c.execute("SELECT 1"));
        QVERIFY(!c.cancel());
        QCOMPARE(c.serverVersion(), 0);
        QCOMPARE(c.backendPid(), 0);
    }

    // Connecting.

    void connectToServer()
    {
        REQUIRE_SERVER();
        Connection c;
        QSignalSpy states(&c, &Connection::stateChanged);
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        QCOMPARE(states.size(), 2);
        QCOMPARE(states[0][0].value<State>(), State::Connecting);
        QCOMPARE(states[1][0].value<State>(), State::Ready);
        QVERIFY(c.serverVersion() >= 140000);
        QVERIFY(c.backendPid() > 0);
        QVERIFY(c.errorMessage().isEmpty());
    }

    void wrongPassword()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo + " password=wrong");
        QVERIFY(waitForState(c, State::Failed));
        QVERIFY2(c.errorMessage().contains(QLatin1String("password authentication failed")),
                 qPrintable(c.errorMessage()));
    }

    void missingDatabase()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo + " dbname=no_such_database");
        QVERIFY(waitForState(c, State::Failed));
        QVERIFY2(c.errorMessage().contains(QLatin1String("does not exist")),
                 qPrintable(c.errorMessage()));
    }

    void reopen()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        const int firstPid = c.backendPid();

        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QVERIFY(c.backendPid() != firstPid);

        c.close();
        QCOMPARE(c.state(), State::Disconnected);
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
    }

    // Queries.

    void query()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        QSignalSpy states(&c, &Connection::stateChanged);
        const auto results = run(c, "SELECT 1 AS one, NULL::text AS nothing, 'žluťoučký' AS text");
        QCOMPARE(c.state(), State::Ready);
        QCOMPARE(states.size(), 2); // Busy, Ready.

        QCOMPARE(results.size(), 1u);
        const Result &r = results[0];
        QCOMPARE(r.status(), PGRES_TUPLES_OK);
        QCOMPARE(r.rowCount(), 1);
        QCOMPARE(r.columnCount(), 3);
        QCOMPARE(r.columnName(0), QStringLiteral("one"));
        QCOMPARE(r.value(0, 0), QByteArray("1"));
        QVERIFY(r.isNull(0, 1));
        QCOMPARE(QString::fromUtf8(r.value(0, 2)), QStringLiteral("žluťoučký"));
        QCOMPARE(r.commandTag(), QByteArray("SELECT 1"));
    }

    void multipleStatements()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const auto results = run(c,
                                 "CREATE TEMP TABLE t (a int); INSERT INTO t VALUES (1), (2); "
                                 "SELECT sum(a) FROM t");
        QCOMPARE(results.size(), 3u);
        QCOMPARE(results[0].commandTag(), QByteArray("CREATE TABLE"));
        QCOMPARE(results[1].commandTag(), QByteArray("INSERT 0 2"));
        QCOMPARE(results[2].value(0, 0), QByteArray("3"));
    }

    void notices()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        QSignalSpy notices(&c, &Connection::notice);
        QCOMPARE(run(c, "DO $$ BEGIN RAISE NOTICE 'hello'; RAISE WARNING 'careful'; END $$").size(),
                 1u);
        QTRY_COMPARE(notices.size(), 2);
        QCOMPARE(notices[0][0].toString(), QStringLiteral("NOTICE:  hello"));
        QCOMPARE(notices[1][0].toString(), QStringLiteral("WARNING:  careful"));
    }

    void busyConnectionRejectsQueries()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        Recorder recorder(c);
        QVERIFY(c.execute("SELECT pg_sleep(0.2)"));
        QCOMPARE(c.state(), State::Busy);
        QVERIFY(!c.execute("SELECT 1"));
        QTRY_COMPARE(recorder.finished, 1);
        QCOMPARE(recorder.results.size(), 1u);
    }

    void chunkedRows()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const auto results = run(c, "SELECT generate_series(1, 2500)", 1000);
        QCOMPARE(results.size(), 4u);
        QVERIFY(results[0].isChunk());
        QCOMPARE(results[0].rowCount(), 1000);
        QCOMPARE(results[0].value(0, 0), QByteArray("1"));
        QCOMPARE(results[1].rowCount(), 1000);
        QCOMPARE(results[2].rowCount(), 500);
        QCOMPARE(results[2].value(499, 0), QByteArray("2500"));
        QCOMPARE(results[3].status(), PGRES_TUPLES_OK);
        QCOMPARE(results[3].rowCount(), 0);
    }

    void largeQueryText()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        // Larger than socket buffers, so it cannot be sent in one go.
        const QByteArray literal(8 * 1024 * 1024, 'x');
        const auto results = run(c, "SELECT length('" + literal + "')");
        QCOMPARE(results.size(), 1u);
        QCOMPARE(results[0].value(0, 0), QByteArray::number(literal.size()));
    }

    void error()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        auto results = run(c, "SELECT * FROM missing_table");
        QCOMPARE(results.size(), 1u);
        QVERIFY(results[0].isError());
        QCOMPARE(results[0].sqlState(), QByteArray("42P01"));
        QCOMPARE(results[0].errorPosition(), 15);
        QVERIFY(results[0].errorMessage().contains(QLatin1String("missing_table")));
        QCOMPARE(c.state(), State::Ready);

        results = run(c, "SELECT 1");
        QCOMPARE(results.size(), 1u);
        QCOMPARE(results[0].value(0, 0), QByteArray("1"));
    }

    void errorEndsMultipleStatements()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const auto results = run(c, "SELECT 1; SELECT 1 / 0; SELECT 3");
        QCOMPARE(results.size(), 2u);
        QCOMPARE(results[0].value(0, 0), QByteArray("1"));
        QCOMPARE(results[1].sqlState(), QByteArray("22012"));
        QCOMPARE(c.state(), State::Ready);
    }

    void copyFromStdinWithoutDataFails()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const auto results = run(c, "CREATE TEMP TABLE t (a int); COPY t FROM STDIN");
        QCOMPARE(c.state(), State::Ready);
        QCOMPARE(results.size(), 3u);
        QCOMPARE(results[1].status(), PGRES_COPY_IN);
        QVERIFY(results[2].isError());
        QVERIFY2(results[2].errorMessage().contains(QLatin1String("No data for COPY")),
                 qPrintable(results[2].errorMessage()));
    }

    void copyFromStdin_data()
    {
        QTest::addColumn<QByteArray>("sql");
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QByteArray>("expectedRows");

        QTest::newRow("text") << QByteArray("COPY t FROM STDIN")
                              << QByteArray("1\tone\n2\t\\N\n3\ttab\\there\n")
                              << QByteArray("1:one|2:NULL|3:tab\there");
        QTest::newRow("empty") << QByteArray("COPY t FROM STDIN") << QByteArray("") << QByteArray();
        QTest::newRow("no final newline")
            << QByteArray("COPY t FROM STDIN") << QByteArray("1\tone\n2\ttwo")
            << QByteArray("1:one|2:two");
        QTest::newRow("CSV with CRLF")
            << QByteArray("COPY t (a, b) FROM STDIN WITH (FORMAT csv, HEADER)")
            << QByteArray("a,b\r\n1,\"x;y\"\r\n2,\"multi\r\nline\"\r\n")
            << QByteArray("1:x;y|2:multi\r\nline");
        QTest::newRow("UTF-8") << QByteArray("COPY t FROM STDIN")
                               << QStringLiteral("1\tžluťoučký kůň\n").toUtf8()
                               << QStringLiteral("1:žluťoučký kůň").toUtf8();
    }

    void copyFromStdin()
    {
        QFETCH(QByteArray, sql);
        QFETCH(QByteArray, data);
        QFETCH(QByteArray, expectedRows);
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        const auto results = run(c, sql, 0, data);
        QCOMPARE(c.state(), State::Ready);
        QCOMPARE(results.size(), 2u);
        QCOMPARE(results[0].status(), PGRES_COPY_IN);
        QVERIFY2(!results[1].isError(), qPrintable(results[1].errorMessage()));
        const int rowCount = expectedRows.isEmpty() ? 0 : int(expectedRows.count('|')) + 1;
        QCOMPARE(results[1].commandTag(), "COPY " + QByteArray::number(rowCount));

        const auto rows = run(c,
                              "SELECT string_agg(a || ':' || coalesce(b, 'NULL'), '|' ORDER BY a) "
                              "FROM t");
        QCOMPARE(rows[0].value(0, 0), expectedRows);
    }

    void copyFromStdinLarge()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        // Far more than libpq and socket buffers hold, so sending has to wait.
        QByteArray data;
        const QByteArray padding(100, 'x');
        for (int i = 1; i <= 200'000; ++i)
            data += QByteArray::number(i) + '\t' + padding + '\n';

        const auto results = run(c, "COPY t FROM STDIN", 0, data);
        QCOMPARE(results.size(), 2u);
        QCOMPARE(results[1].commandTag(), QByteArray("COPY 200000"));
        QCOMPARE(run(c, "SELECT sum(a) FROM t")[0].value(0, 0), QByteArray("20000100000"));
    }

    void copyFromStdinBadData()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        // The bad row comes first; the rest is still sent and discarded.
        QByteArray data = "oops\tbad\n";
        for (int i = 1; i <= 200'000; ++i)
            data += QByteArray::number(i) + "\tfine\n";

        const auto results = run(c, "COPY t FROM STDIN", 0, data);
        QCOMPARE(c.state(), State::Ready);
        QCOMPARE(results.size(), 2u);
        QCOMPARE(results[1].sqlState(), QByteArray("22P02"));
        QCOMPARE(run(c, "SELECT count(*) FROM t")[0].value(0, 0), QByteArray("0"));
    }

    void copyDataGoesToFirstCopyOnly()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        const auto results = run(c, "COPY t FROM STDIN; COPY t FROM STDIN", 0, "1\tone\n");
        QCOMPARE(results.size(), 4u);
        QCOMPARE(results[1].commandTag(), QByteArray("COPY 1"));
        QCOMPARE(results[2].status(), PGRES_COPY_IN);
        QVERIFY(results[3].errorMessage().contains(QLatin1String("No data for COPY")));
        QCOMPARE(c.state(), State::Ready);
    }

    void cancelCopyFromStdin()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        QByteArray data;
        for (int i = 1; i <= 2'000'000; ++i)
            data += QByteArray::number(i) + "\tfine\n";

        Recorder recorder(c);
        QVERIFY(c.execute("COPY t FROM STDIN", 0, data));
        QTRY_VERIFY(!recorder.results.empty()); // COPY_IN: sending has started.
        QVERIFY(c.cancel());
        QTRY_COMPARE_WITH_TIMEOUT(recorder.finished, 1, 30'000);
        QCOMPARE(c.state(), State::Ready);
        // Either the cancel arrived in time, or the COPY had already finished.
        const Result &last = recorder.results.back();
        QVERIFY2(last.sqlState() == "57014" || last.commandTag() == "COPY 2000000",
                 qPrintable(last.errorMessage()));
    }

    void terminatedDuringCopy()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "CREATE TEMP TABLE t (a int, b text)").size(), 1u);

        QByteArray data;
        for (int i = 1; i <= 2'000'000; ++i)
            data += QByteArray::number(i) + "\tfine\n";

        Connection admin;
        admin.open(conninfo);
        QVERIFY(waitForState(admin, State::Ready));

        Recorder recorder(c);
        QVERIFY(c.execute("COPY t FROM STDIN", 0, data));
        QTRY_VERIFY(!recorder.results.empty()); // COPY_IN: sending has started.
        run(admin, "SELECT pg_terminate_backend(" + QByteArray::number(c.backendPid()) + ")");

        // Whether the COPY was still sending or had just finished, the
        // connection ends up failed rather than hanging.
        QVERIFY(waitForState(c, State::Failed, 10'000));
        QVERIFY(!c.errorMessage().isEmpty());
        QCOMPARE(recorder.finished, 0);
    }

    // Runs a pg_dump-style script the way the editor does: statement by
    // statement, with each COPY getting the data span that follows it.
    void runSplitScript()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const QByteArray script = "CREATE TEMP TABLE orders (id int PRIMARY KEY, note text);\n"
                                  "COPY orders (id, note) FROM stdin;\n"
                                  "1\tfirst; with semicolon\n"
                                  "2\t\\N\n"
                                  "\\.\n"
                                  "\\echo loaded\n"
                                  "INSERT INTO orders VALUES (3, 'third');\n"
                                  "SELECT count(*), count(note) FROM orders;\n";

        using Kind = slonisko::sql::StatementSpan::Kind;
        const auto spans = slonisko::sql::splitStatements(script);
        std::vector<Result> last;
        for (std::size_t i = 0; i < spans.size(); ++i) {
            if (spans[i].kind != Kind::Sql)
                continue; // Meta-commands are the script runner's business.
            std::optional<QByteArray> data;
            if (i + 1 < spans.size() && spans[i + 1].kind == Kind::CopyData)
                data = script.mid(spans[i + 1].offset, spans[i + 1].length);
            last = run(c, script.mid(spans[i].offset, spans[i].length), 0, data);
            QVERIFY(!last.empty());
            QVERIFY2(!last.back().isError(), qPrintable(last.back().errorMessage()));
        }
        QCOMPARE(last[0].value(0, 0), QByteArray("3"));
        QCOMPARE(last[0].value(0, 1), QByteArray("2"));
    }

    void copyToStdoutIsDiscarded()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        const auto results = run(c, "COPY (SELECT generate_series(1, 100000)) TO STDOUT");
        QCOMPARE(c.state(), State::Ready);
        QCOMPARE(results.size(), 2u);
        QCOMPARE(results[0].status(), PGRES_COPY_OUT);
        QCOMPARE(results[1].commandTag(), QByteArray("COPY 100000"));
    }

    // Interrupting.

    void cancel()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QVERIFY(!c.cancel()); // Nothing to cancel.

        Recorder recorder(c);
        QSignalSpy cancelFailed(&c, &Connection::cancelFailed);
        QVERIFY(c.execute("SELECT pg_sleep(30)"));
        QVERIFY(waitUntilSleeping(conninfo, c.backendPid()));

        QElapsedTimer timer;
        timer.start();
        QVERIFY(c.cancel());
        QVERIFY(!c.cancel()); // Already in progress.
        QTRY_COMPARE_WITH_TIMEOUT(recorder.finished, 1, 5'000);
        QVERIFY(timer.elapsed() < 5'000);

        QCOMPARE(recorder.results.size(), 1u);
        QCOMPARE(recorder.results[0].sqlState(), QByteArray("57014"));
        QCOMPARE(cancelFailed.size(), 0);
        QCOMPARE(c.state(), State::Ready);

        // The connection is still usable, including for another cancel.
        QCOMPARE(run(c, "SELECT 1").size(), 1u);
        QVERIFY(c.execute("SELECT pg_sleep(30)"));
        QVERIFY(waitUntilSleeping(conninfo, c.backendPid()));
        QVERIFY(c.cancel());
        QVERIFY(waitForState(c, State::Ready, 5'000));
    }

    void terminatedByServer()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        Recorder recorder(c);
        QVERIFY(c.execute("SELECT pg_sleep(30)"));
        QVERIFY(waitUntilSleeping(conninfo, c.backendPid()));

        Connection admin;
        admin.open(conninfo);
        QVERIFY(waitForState(admin, State::Ready));
        run(admin, "SELECT pg_terminate_backend(" + QByteArray::number(c.backendPid()) + ")");

        QVERIFY(waitForState(c, State::Failed, 5'000));
        QVERIFY(!c.errorMessage().isEmpty());
        QCOMPARE(recorder.finished, 0);
        QVERIFY(!recorder.results.empty());
        QCOMPARE(recorder.results[0].sqlState(), QByteArray("57P01"));
        QVERIFY(!c.execute("SELECT 1"));
    }

    void closeDuringQuery()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));

        Recorder recorder(c);
        QVERIFY(c.execute("SELECT pg_sleep(30)"));
        QVERIFY(c.cancel());
        c.close();
        QCOMPARE(c.state(), State::Disconnected);

        QTest::qWait(500);
        QCOMPARE(recorder.finished, 0);
        QVERIFY(recorder.results.empty());
    }

    void deleteFromSignal()
    {
        REQUIRE_SERVER();
        auto *c = new Connection;
        c->open(conninfo);
        QVERIFY(waitForState(*c, State::Ready));

        bool deleted = false;
        QObject::connect(c, &Connection::resultReady, c, [&] {
            deleted = true;
            delete c;
        });
        QVERIFY(c->execute("SELECT 1; SELECT 2"));
        QTRY_VERIFY(deleted);
        QTest::qWait(100); // Nothing may touch the deleted connection.
    }

    // Timeouts.

    void statementTimeout()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "SET statement_timeout = 200").size(), 1u);

        QElapsedTimer timer;
        timer.start();
        const auto results = run(c, "SELECT pg_sleep(5)");
        QVERIFY(timer.elapsed() < 3'000);
        QCOMPARE(results.size(), 1u);
        QCOMPARE(results[0].sqlState(), QByteArray("57014"));
        QVERIFY2(results[0].errorMessage().contains(QLatin1String("statement timeout")),
                 qPrintable(results[0].errorMessage()));
        QCOMPARE(c.state(), State::Ready);
    }

    void idleSessionTimeout()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QVERIFY(waitForState(c, State::Ready));
        QCOMPARE(run(c, "SET idle_session_timeout = 300").size(), 1u);

        // Noticed without any query running.
        QVERIFY(waitForState(c, State::Failed, 5'000));
        QVERIFY2(c.errorMessage().contains(QLatin1String("idle-session timeout")),
                 qPrintable(c.errorMessage()));
    }

private:
    // Waits until the backend is inside pg_sleep(), so that a cancel request
    // cannot arrive before the query starts and be ignored.
    static bool waitUntilSleeping(const QByteArray &conninfo, int pid)
    {
        Connection monitor;
        monitor.open(conninfo);
        if (!waitForState(monitor, State::Ready))
            return false;
        const QByteArray sql
            = "SELECT wait_event FROM pg_stat_activity WHERE pid = " + QByteArray::number(pid);
        return QTest::qWaitFor(
            [&] {
                const auto results = run(monitor, sql);
                return !results.empty() && results[0].rowCount() == 1
                    && results[0].value(0, 0) == "PgSleep";
            },
            5'000);
    }
};

QTEST_GUILESS_MAIN(TestConnection)
#include "tst_connection.moc"
