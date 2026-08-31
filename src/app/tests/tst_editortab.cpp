// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"
#include "MainWindow.h"
#include "EditorTab.h"
#include "PlanView.h"
#include "ResultModel.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"
#include "TestServer.h"

#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace slonisko;

class TestEditorTab : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_server = TestServer::find();
        if (!m_server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        m_settings.emplace(m_dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
        m_browser = std::make_unique<ConnectionBrowser>(*m_settings, false);
        m_session = std::make_unique<Session>(m_server->profile,
                                              Session::Credentials {m_server->password, {}});
        m_session->open();
        QTRY_COMPARE(m_session->state(), Session::State::Connected);
    }

    void init()
    {
        m_tab = std::make_unique<EditorTab>(m_browser.get(), QStringLiteral("Script"));
        m_tab->setSession(m_session.get());
        QTRY_COMPARE(m_tab->connection()->state(), pg::Connection::State::Ready);
        QVERIFY(m_tab->title().contains(QLatin1String("Test")));
    }

    void cleanup() { m_tab.reset(); }

    void runsCurrentStatement()
    {
        setText("SELECT 1;\nSELECT g, 'x' AS t FROM generate_series(1, 3) g;\n");
        cursorAt("generate_series");
        runAndWait();
        QCOMPARE(model()->rowCount(), 3);
        QCOMPARE(model()->columnCount(), 2);
        QCOMPARE(model()->headerData(1, Qt::Horizontal).toString(), QStringLiteral("t"));
        QVERIFY2(messages().contains(QLatin1String(": 3 row(s)")), qPrintable(messages()));
    }

    void runsSelection()
    {
        setText(
            "CREATE TEMP TABLE sel (a int); INSERT INTO sel VALUES (1), (2); SELECT * FROM sel;");
        m_tab->editor()->selectAll();
        runAndWait();
        QCOMPARE(model()->rowCount(), 2);
        QVERIFY(messages().contains(QLatin1String("INSERT 0 2")));
    }

    void errorIsMarked()
    {
        // Non-ASCII before the error: PostgreSQL counts characters, the editor bytes.
        setText("SELECT 1;\nSELECT 'žluťoučký', nope FROM (SELECT 1) s;");
        cursorAt("nope");
        runAndWait();
        const qsizetype pos = find("nope");
        QVERIFY(m_tab->editor()->hasErrorAt(pos));
        QVERIFY(!m_tab->editor()->hasErrorAt(pos - 2));
        QVERIFY(messages().contains(QLatin1String("nope")));
    }

    void errorStopsTheRest()
    {
        setText("SELECT 1/0; SELECT 'never';");
        m_tab->editor()->selectAll();
        runAndWait();
        QVERIFY(messages().contains(QLatin1String("division by zero")));
        QVERIFY(!messages().contains(QLatin1String("never")));
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);
    }

    void rowLimit()
    {
        m_tab->setRowLimit(500);
        setText("SELECT g FROM generate_series(1, 1000000) g;");
        runAndWait();
        QVERIFY(model()->rowCount() >= 500);
        QVERIFY(model()->rowCount() < 1000000);
        QVERIFY(messages().contains(QLatin1String("stopped after")));
        QCOMPARE(m_tab->connection()->state(), pg::Connection::State::Ready);
    }

    void copyWithData()
    {
        setText("CREATE TEMP TABLE c (a int, b text);\nCOPY c FROM stdin;\n1\tone\n2\ttwo\n\\.\n"
                "SELECT * FROM c ORDER BY a;");
        m_tab->editor()->selectAll();
        runAndWait();
        QCOMPARE(model()->rowCount(), 2);
        QVERIFY(messages().contains(QLatin1String("COPY 2")));
    }

    void psqlCommandsAreSkipped()
    {
        setText("\\set x 1\nSELECT 1;");
        m_tab->editor()->selectAll();
        runAndWait();
        QVERIFY(messages().contains(QLatin1String("Skipped psql command")));
        QCOMPARE(model()->rowCount(), 1);
    }

    void cancel()
    {
        setText("SELECT pg_sleep(30);");
        m_tab->run();
        QVERIFY(m_tab->isRunning());
        QTest::qWait(300);
        QElapsedTimer timer;
        timer.start();
        m_tab->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!m_tab->isRunning(), 5'000);
        QVERIFY(timer.elapsed() < 5'000);
        QVERIFY(messages().contains(QLatin1String("canceling statement")));
    }

    void explain()
    {
        setText("SELECT * FROM generate_series(1, 10) g WHERE g > 5;");
        m_tab->explain(false);
        QTRY_VERIFY(!m_tab->isRunning());
        const PlanModel *plan = m_tab->resultPanel()->plan()->model();
        QCOMPARE(plan->rowCount(), 1);
        QCOMPARE(plan->index(0, PlanModel::Operation).data().toString(),
                 QStringLiteral("Function Scan"));
        QVERIFY(!plan->plan().analyzed);
    }

    void explainAnalyzeRollsBack()
    {
        setText("CREATE TEMP TABLE ea (a int);");
        runAndWait();

        setText("INSERT INTO ea SELECT generate_series(1, 100);");
        m_tab->explain(true);
        QTRY_VERIFY(!m_tab->isRunning());
        QVERIFY(m_tab->resultPanel()->plan()->model()->plan().analyzed);
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);

        setText("SELECT count(*) FROM ea;");
        runAndWait();
        QCOMPARE(model()->index(0, 0).data().toString(), QStringLiteral("0"));
    }

    void explainAnalyzeInsideTransaction()
    {
        setText("BEGIN; CREATE TEMP TABLE et (a int); INSERT INTO et VALUES (1);");
        m_tab->editor()->selectAll();
        runAndWait();
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);

        setText("INSERT INTO et VALUES (2);");
        m_tab->explain(true);
        QTRY_VERIFY(!m_tab->isRunning());
        // Still in the user's transaction, without the explained insert.
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);
        setText("SELECT count(*) FROM et;");
        runAndWait();
        QCOMPARE(model()->index(0, 0).data().toString(), QStringLiteral("1"));

        setText("ROLLBACK;");
        runAndWait();
    }

    void explainErrorIsMarked()
    {
        setText("SELECT nope FROM (SELECT 1) s;");
        m_tab->explain(false);
        QTRY_VERIFY(!m_tab->isRunning());
        // The position is in the user's text, not in the EXPLAIN prefix.
        QVERIFY(m_tab->editor()->hasErrorAt(find("nope")));
    }

    // Regression: closing the window with several editors crashed.
    void mainWindowClosesWithEditors()
    {
        QStandardPaths::setTestModeEnabled(true); // Keep away from the real settings.
        auto window = std::make_unique<MainWindow>();
        window->newEditor(m_session.get());
        window->newEditor(m_session.get());
        window->show();
        QTest::qWait(100);
        window.reset();
    }

    void sessionGoingAwayDisconnects()
    {
        auto session = std::make_unique<Session>(m_server->profile,
                                                 Session::Credentials {m_server->password, {}});
        session->open();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        m_tab->setSession(session.get());
        QTRY_COMPARE(m_tab->connection()->state(), pg::Connection::State::Ready);
        session->close();
        Q_EMIT m_browser->sessionsChanged();
        QCOMPARE(m_tab->connection()->state(), pg::Connection::State::Disconnected);
        QVERIFY(!m_tab->session());
    }

private:
    void setText(const char *text)
    {
        m_tab->editor()->setText(QString::fromUtf8(text));
        m_tab->editor()->setCursorPosition(0);
    }

    qsizetype find(const char *text) const { return m_tab->editor()->utf8Text().indexOf(text); }

    void cursorAt(const char *text) { m_tab->editor()->setCursorPosition(find(text)); }

    void runAndWait()
    {
        m_tab->run();
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
    }

    ResultModel *model() const { return m_tab->resultPanel()->results()->model(); }
    QString messages() const { return m_tab->resultPanel()->messages()->toPlainText(); }

    std::optional<TestServer> m_server;
    QTemporaryDir m_dir;
    std::optional<QSettings> m_settings;
    std::unique_ptr<ConnectionBrowser> m_browser;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<EditorTab> m_tab;
};

QTEST_MAIN(TestEditorTab)
#include "tst_editortab.moc"
