// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/QueryRunner.h"

#include <QFile>
#include <QTest>

#include <memory>

using slonisko::pg::Connection;
using slonisko::pg::QueryOutcome;
using slonisko::pg::QueryRunner;
using State = Connection::State;

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

#define REQUIRE_SERVER()                                                                           \
    const QByteArray conninfo = serverConninfo();                                                  \
    if (conninfo.isEmpty())                                                                        \
        QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");

} // namespace

class TestQueryRunner : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void queuedInOrderBeforeConnecting()
    {
        REQUIRE_SERVER();
        Connection c;
        QueryRunner runner(&c);
        QStringList order;
        for (int i = 1; i <= 3; ++i) {
            runner.run("SELECT " + QByteArray::number(i), this, [&, i](const QueryOutcome &o) {
                QVERIFY(o.ok());
                QCOMPARE(o.results.size(), 1u);
                order << QString::fromUtf8(o.results[0].value(0, 0));
                QCOMPARE(order.last(), QString::number(i));
            });
        }
        QCOMPARE(runner.pending(), 3);
        c.open(conninfo); // Queries wait for the connection.
        QTRY_COMPARE(order.size(), 3);
        QCOMPARE(order,
                 (QStringList {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}));
        QCOMPARE(runner.pending(), 0);
    }

    void errorDoesNotStopTheQueue()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QueryRunner runner(&c);
        std::optional<QueryOutcome> failed, next;
        runner.run("SELECT * FROM missing_table", this, [&](const QueryOutcome &o) { failed = o; });
        runner.run("SELECT 2", this, [&](const QueryOutcome &o) { next = o; });
        QTRY_VERIFY(next.has_value());
        QVERIFY(!failed->ok());
        QCOMPARE(failed->sqlState, QByteArray("42P01"));
        QVERIFY(failed->error.contains(QLatin1String("missing_table")));
        QVERIFY(next->ok());
    }

    void callbackSkippedWhenContextDeleted()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QueryRunner runner(&c);
        bool called = false, after = false;
        auto context = std::make_unique<QObject>();
        runner.run("SELECT pg_sleep(0.1)", context.get(),
                   [&](const QueryOutcome &) { called = true; });
        runner.run("SELECT 1", this, [&](const QueryOutcome &) { after = true; });
        context.reset();
        QTRY_VERIFY(after);
        QVERIFY(!called);
    }

    void callbackMayQueueMore()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QueryRunner runner(&c);
        QByteArray value;
        runner.run("SELECT 1", this, [&](const QueryOutcome &) {
            runner.run("SELECT 'nested'", this,
                       [&](const QueryOutcome &o) { value = o.results[0].value(0, 0); });
        });
        QTRY_COMPARE(value, QByteArray("nested"));
    }

    void callbackMayDeleteRunner()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        auto *runner = new QueryRunner(&c);
        bool deleted = false;
        runner->run("SELECT 1", this, [&](const QueryOutcome &) {
            delete runner;
            deleted = true;
        });
        runner->run("SELECT 2", this, [](const QueryOutcome &) { });
        QTRY_VERIFY(deleted);
        QTest::qWait(100);
        QTRY_COMPARE(c.state(), State::Ready);
    }

    void connectionFailureFailsEverything()
    {
        REQUIRE_SERVER();
        Connection c;
        QueryRunner runner(&c);
        int failures = 0;
        for (int i = 0; i < 3; ++i) {
            runner.run("SELECT 1", this, [&](const QueryOutcome &o) {
                QVERIFY(!o.ok());
                QVERIFY(o.error.contains(QLatin1String("password")));
                ++failures;
            });
        }
        c.open(conninfo + " password=wrong");
        QTRY_COMPARE(failures, 3);
        QCOMPARE(runner.pending(), 0);

        // Queries on a failed connection fail too, asynchronously.
        bool failed = false;
        runner.run("SELECT 1", this, [&](const QueryOutcome &o) { failed = !o.ok(); });
        QVERIFY(!failed);
        QTRY_VERIFY(failed);
    }

    void closeFailsRunningQuery()
    {
        REQUIRE_SERVER();
        Connection c;
        c.open(conninfo);
        QueryRunner runner(&c);
        std::optional<QueryOutcome> outcome;
        runner.run("SELECT pg_sleep(30)", this, [&](const QueryOutcome &o) { outcome = o; });
        QTRY_COMPARE(c.state(), State::Busy);
        c.close();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->error, QStringLiteral("Disconnected"));
    }
};

QTEST_GUILESS_MAIN(TestQueryRunner)
#include "tst_queryrunner.moc"
