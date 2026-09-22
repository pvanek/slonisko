// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"
#include "MainWindow.h"
#include "ResultPage.h"
#include "EditorPage.h"
#include "ObjectPage.h"
#include "PlanView.h"
#include "ResultModel.h"
#include "ResultPanel.h"
#include "ResultTextView.h"
#include "ResultView.h"
#include "catalog/Export.h"
#include "Session.h"
#include "SqlEditor.h"
#include "TestServer.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QComboBox>
#include <QSplitter>
#include <QTabBar>
#include <QToolButton>
#include <QTableView>
#include <QTabWidget>
#include <QFile>
#include <QClipboard>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace slonisko;

class TestEditorPage : public QObject
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
        m_tab = std::make_unique<EditorPage>(m_browser.get(), QStringLiteral("Script"));
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
        setText("\\connect other\nSELECT 1;");
        m_tab->editor()->selectAll();
        runAndWait();
        QVERIFY(messages().contains(QLatin1String("Skipped psql command")));
        QCOMPARE(model()->rowCount(), 1);
    }

    void psqlVariables()
    {
        setText("\\set n 3\n\\set who 'O''Brien'\n\\echo counting to :n\n"
                "SELECT g, :'who' AS who FROM generate_series(1, :n) g;");
        m_tab->editor()->selectAll();
        runAndWait();
        QCOMPARE(model()->rowCount(), 3);
        QCOMPARE(model()->index(0, 1).data().toString(), QStringLiteral("O'Brien"));
        QVERIFY(messages().contains(QLatin1String("counting to 3")));

        // They stay set for later runs in this editor, like in psql.
        setText("SELECT :n + 1;");
        runAndWait();
        QCOMPARE(model()->index(0, 0).data().toString(), QStringLiteral("4"));

        // Error positions point into the text as written, not as sent.
        setText("\\set long 'a much longer value'\nSELECT :'long', nope FROM (SELECT 1) s;");
        m_tab->editor()->selectAll();
        runAndWait();
        QVERIFY(m_tab->editor()->hasErrorAt(find("nope")));

        setText("SELECT :undefined_var;");
        runAndWait();
        QVERIFY(messages().contains(QLatin1String(":undefined_var is not set")));
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

    void explainAsText()
    {
        setText("SELECT * FROM generate_series(1, 3)");
        m_tab->explain(false);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
        PlanView *plan = m_tab->resultPanel()->plan();
        QCOMPARE(plan->viewMode(), PlanView::ViewMode::Tree);

        plan->modeTabs()->setCurrentIndex(1);
        QCOMPARE(plan->viewMode(), PlanView::ViewMode::Text);
        QVERIFY(plan->textView()->isVisibleTo(plan));
        const QString text = plan->textView()->text();
        QVERIFY2(text.contains(QLatin1String("Function Scan")), qPrintable(text));
        QVERIFY(text.contains(QLatin1String("(cost=")));

        // A failed explain leaves no stale plan text behind.
        setText("SELECT * FROM no_such_table_here");
        m_tab->explain(false);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
        QVERIFY(plan->textView()->text().isEmpty());
    }

    void explainErrorIsMarked()
    {
        setText("SELECT nope FROM (SELECT 1) s;");
        m_tab->explain(false);
        QTRY_VERIFY(!m_tab->isRunning());
        // The position is in the user's text, not in the EXPLAIN prefix.
        QVERIFY(m_tab->editor()->hasErrorAt(find("nope")));
    }

    void editAndSave()
    {
        setText("CREATE TEMP TABLE ed (id int PRIMARY KEY, name text, note text);\n"
                "INSERT INTO ed VALUES (1, 'one', 'a'), (2, 'two', 'b'), (3, 'three', 'c');");
        m_tab->editor()->selectAll();
        runAndWait();

        setText("SELECT id, name, note, upper(name) AS shout FROM ed ORDER BY id;");
        runAndWait();
        QTRY_VERIFY(model()->isEditable());
        QCOMPARE(model()->editTarget().table, QStringLiteral("ed"));
        QVERIFY(!(model()->flags(model()->index(0, 3)) & Qt::ItemIsEditable)); // Computed.

        QVERIFY(model()->setData(model()->index(0, 1), QStringLiteral("uno")));
        model()->setNull({model()->index(0, 2)});
        model()->toggleDeleted({1});
        const int added = model()->addRow();
        QVERIFY(model()->setData(model()->index(added, 0), QStringLiteral("10")));
        QVERIFY(model()->setData(model()->index(added, 1), QStringLiteral("ten")));
        QVERIFY(model()->hasChanges());

        m_tab->saveChanges(false);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
        QVERIFY2(messages().contains(QLatin1String("Saved 3 change(s).")), qPrintable(messages()));
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);

        // The query ran again, showing the saved rows.
        QVERIFY(!model()->hasChanges());
        QCOMPARE(model()->rowCount(), 3);
        QCOMPARE(model()->index(0, 1).data().toString(), QStringLiteral("uno"));
        QCOMPARE(model()->index(0, 2).data().toString(), QStringLiteral("NULL"));
        QCOMPARE(model()->index(1, 0).data().toString(), QStringLiteral("3"));
        QCOMPARE(model()->index(2, 1).data().toString(), QStringLiteral("ten"));
    }

    void failedSaveKeepsEverything()
    {
        setText("CREATE TEMP TABLE fs (id int PRIMARY KEY, name text);\n"
                "INSERT INTO fs VALUES (1, 'one'), (2, 'two');");
        m_tab->editor()->selectAll();
        runAndWait();
        setText("SELECT * FROM fs ORDER BY id;");
        runAndWait();
        QTRY_VERIFY(model()->isEditable());

        QVERIFY(model()->setData(model()->index(0, 1), QStringLiteral("changed")));
        const int added = model()->addRow();
        QVERIFY(model()->setData(model()->index(added, 0), QStringLiteral("2"))); // Duplicate key.
        m_tab->saveChanges(false);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
        QVERIFY(messages().contains(QLatin1String("duplicate key")));
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE); // Rolled back.
        QVERIFY(model()->hasChanges()); // Kept, to fix and save again.

        setText("SELECT name FROM fs WHERE id = 1;");
        runAndWait();
        QCOMPARE(model()->index(0, 0).data().toString(), QStringLiteral("one"));
    }

    void saveInsideTransaction()
    {
        setText("BEGIN; CREATE TEMP TABLE st (id int PRIMARY KEY, v int); INSERT INTO st VALUES "
                "(1, 1);");
        m_tab->editor()->selectAll();
        runAndWait();
        setText("SELECT * FROM st;");
        runAndWait();
        QTRY_VERIFY(model()->isEditable());
        QVERIFY(model()->setData(model()->index(0, 1), QStringLiteral("5")));
        m_tab->saveChanges(false);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));
        QVERIFY(messages().contains(QLatin1String("in the open transaction")));
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);
        QCOMPARE(model()->index(0, 1).data().toString(), QStringLiteral("5"));
        setText("ROLLBACK;");
        runAndWait();
    }

    void readOnlyResults()
    {
        setText("SELECT 1 AS one;");
        runAndWait();
        QVERIFY(!model()->isEditable());
        QVERIFY(model()->editTarget().reason.contains(QLatin1String("not come from a table")));
        QVERIFY(!(model()->flags(model()->index(0, 0)) & Qt::ItemIsEditable));
    }

    void files()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("script.sql"));
        QVERIFY(m_tab->isBlank());
        setText("SELECT 'žluťoučký';\n");
        QVERIFY(m_tab->isModified());
        QVERIFY(m_tab->title().startsWith(QLatin1String("Script*")));

        QString error;
        QVERIFY2(m_tab->saveFile(path, &error), qPrintable(error));
        QVERIFY(!m_tab->isModified());
        QVERIFY(m_tab->title().startsWith(QStringLiteral("script.sql ·")));
        QCOMPARE(m_tab->filePath(), path);
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QStringLiteral("SELECT 'žluťoučký';\n").toUtf8());

        // Opened elsewhere, the same text; not modified.
        EditorPage other(m_browser.get(), QStringLiteral("Other"));
        QVERIFY(other.openFile(path, &error));
        QCOMPARE(other.editor()->utf8Text(), QStringLiteral("SELECT 'žluťoučký';\n").toUtf8());
        QVERIFY(!other.isModified());
        QVERIFY(!other.isBlank());

        // Windows line endings stay Windows line endings, new lines too.
        const QString crlf = dir.filePath(QStringLiteral("crlf.sql"));
        QFile file(crlf);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("SELECT 1;\r\nSELECT 2;\r\n");
        file.close();
        QVERIFY(other.openFile(crlf));
        other.editor()->setCursorPosition(other.editor()->utf8Text().size());
        other.editor()->SendScintilla(QsciScintillaBase::SCI_NEWLINE);
        QVERIFY(other.saveFile(crlf));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("SELECT 1;\r\nSELECT 2;\r\n\r\n"));

        QVERIFY(!other.saveFile(dir.filePath(QStringLiteral("missing/dir/x.sql")), &error));
        QVERIFY(!error.isEmpty());
    }

    void connectionColor()
    {
        auto profile = m_server->profile;
        profile.color = QStringLiteral("#c62828");
        auto red
            = std::make_unique<Session>(profile, Session::Credentials {m_server->password, {}});
        red->open();
        QTRY_COMPARE(red->state(), Session::State::Connected);
        m_tab->setSession(red.get());
        QCOMPARE(m_tab->color(), QColor(0xc6, 0x28, 0x28));

        auto *combo = m_tab->findChild<QComboBox *>();
        QVERIFY(combo);
        QCOMPARE(combo->property("connectionColor").value<QColor>(), QColor(0xc6, 0x28, 0x28));
        // Tinted towards red.
        const QColor button = combo->palette().color(QPalette::Button);
        QVERIFY(button.red() > button.blue());

        m_tab->setSession(nullptr);
        QVERIFY(!combo->property("connectionColor").value<QColor>().isValid());
    }

    void refreshShortcuts()
    {
        setText("SELECT clock_timestamp()::text;");
        runAndWait();
        const QString first = model()->index(0, 0).data().toString();
        QTableView *table = m_tab->resultPanel()->results()->table();
        m_tab->window()->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_tab->window()));
        m_tab->editor()->setText(QStringLiteral("-- something else entirely"));

        // F5 in the results runs their query again, whatever the editor holds now.
        table->setFocus();
        QTest::keyClick(table, Qt::Key_F5);
        QTRY_VERIFY(model()->rowCount() == 1 && model()->index(0, 0).data().toString() != first);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));

        const QString second = model()->index(0, 0).data().toString();
        QTest::keyClick(table, Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(model()->rowCount() == 1 && model()->index(0, 0).data().toString() != second);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000));

        // A statement without rows leaves nothing to run again.
        setText("SELECT 1 WHERE false; CREATE TEMP TABLE nothing_to_show (a int);");
        m_tab->editor()->selectAll();
        runAndWait();
        const int before = model()->rowCount();
        m_tab->resultPanel()->results()->setFocus();
        QTest::keyClick(m_tab->resultPanel()->results(), Qt::Key_F5);
        QTest::qWait(200);
        QVERIFY(!m_tab->isRunning());
        QCOMPARE(model()->rowCount(), before);
    }

    void editorAndResultsAreOnePage()
    {
        // The result panel lives inside the editor page, below the editor.
        QVERIFY(m_tab->isAncestorOf(m_tab->resultPanel()));
        auto *splitter = m_tab->findChild<QSplitter *>();
        QVERIFY(splitter);
        QCOMPARE(splitter->widget(0), m_tab->editor());
        QCOMPARE(splitter->widget(1), static_cast<QWidget *>(m_tab->resultPanel()));
    }

    void resultPages()
    {
        QStandardPaths::setTestModeEnabled(true);
        MainWindow w;
        QCOMPARE(w.pages().size(), 1); // The first editor.

        const QByteArray sql = "SELECT g FROM generate_series(1, 5) g";
        ResultPage *page = w.showResult(m_session.get(), QStringLiteral("Five"), sql);
        QCOMPARE(w.currentPage(), page);
        QVERIFY(!w.currentEditor()); // Not an editor: Save does not apply.
        QCOMPARE(page->title(), QStringLiteral("Five"));
        QTRY_COMPARE(page->results()->model()->rowCount(), 5);
        QVERIFY(!page->results()->model()->isEditable());

        // The same query again: the same page, shown and run again.
        w.newEditor(m_session.get());
        QCOMPARE(w.pages().size(), 3);
        QCOMPARE(w.showResult(m_session.get(), QStringLiteral("Five"), sql), page);
        QCOMPARE(w.currentPage(), page);
        QCOMPARE(w.pages().size(), 3);

        // Another query: a page of its own.
        ResultPage *other = w.showResult(m_session.get(), QStringLiteral("One"), "SELECT 1");
        QVERIFY(other != page);
        QCOMPARE(w.pages().size(), 4);

        // Closing pages; the last one closed brings a fresh editor.
        auto *tabs = qobject_cast<QTabWidget *>(page->parentWidget()->parentWidget());
        QVERIFY(tabs);
        while (tabs->count() > 1)
            QVERIFY(w.closePage(0));
        QVERIFY(w.closePage(0));
        QCOMPARE(w.pages().size(), 1);
        QVERIFY(w.currentEditor());
    }

    void objectOpensDetailsPage()
    {
        QStandardPaths::setTestModeEnabled(true);
        MainWindow w;
        runSql("CREATE TEMP TABLE details_demo (id int PRIMARY KEY, note text NOT NULL)");
        runSql("CREATE INDEX details_demo_note ON details_demo (note)");
        runSql("SELECT 'details_demo'::regclass::oid AS oid");
        const unsigned int oid = model()->index(0, 0).data().toString().toUInt();
        QVERIFY(oid > 0);

        ObjectPage *page = w.showObject(m_session.get(), {}, catalog::ObjectKind::Table, oid,
                                        QStringLiteral("details_demo"));
        QVERIFY(page);
        QTRY_VERIFY(!page->detail().title.isEmpty());
        QVERIFY(page->title().endsWith(QLatin1String("details_demo")));

        // A tab for the overview and one per list, counts in their labels.
        QStringList tabs;
        for (int tab = 0; tab < page->tabs()->count(); ++tab)
            tabs << page->tabs()->tabText(tab);
        QVERIFY2(tabs.first().contains(QLatin1String("Overview")), qPrintable(tabs.join(u',')));
        QVERIFY(tabs.contains(QStringLiteral("Columns (2)")));
        QVERIFY(tabs.contains(QStringLiteral("Indexes (2)")));
        QVERIFY(tabs.contains(QStringLiteral("Constraints (1)")));
        QVERIFY(tabs.contains(QStringLiteral("Triggers"))); // None: no count.

        // The same object again lands on the page it already has.
        QCOMPARE(w.showObject(m_session.get(), {}, catalog::ObjectKind::Table, oid,
                              QStringLiteral("details_demo")),
                 page);

        // The tree asks for it the same way.
        Q_EMIT w.browser()->objectRequested(m_session.get(), QString(),
                                            catalog::ObjectKind::Extension, extensionOid(),
                                            QStringLiteral("plpgsql"));
        auto *extension = qobject_cast<ObjectPage *>(w.currentPage());
        QVERIFY(extension);
        QTRY_COMPARE(extension->detail().title, QStringLiteral("plpgsql"));
        QVERIFY(extension->tabs()->count() >= 2); // Overview and Objects.
    }

    void monitoringOpensResultPage()
    {
        QStandardPaths::setTestModeEnabled(true);
        MainWindow w;
        Q_EMIT w.browser()->monitoringRequested(m_session.get(), QStringLiteral("Sessions"),
                                                "SELECT pid FROM pg_stat_activity");
        auto *page = qobject_cast<ResultPage *>(w.currentPage());
        QVERIFY(page);
        QCOMPARE(page->title(), QStringLiteral("Sessions"));
        QTRY_VERIFY(page->results()->model()->rowCount() > 0);
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

    void commitsBeforeClosing()
    {
        QString asked;
        bool couldCommit = false;
        m_tab->setTransactionPrompt([&](const QString &question, bool canCommit) {
            asked = question;
            couldCommit = canCommit;
            return EditorPage::TransactionChoice::Commit;
        });
        QVERIFY(m_tab->maybeClose()); // Nothing open: no question.
        QVERIFY(asked.isEmpty());

        runSql("BEGIN");
        runSql("CREATE TABLE tx_committed (i int)");
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);
        QVERIFY(m_tab->maybeClose());
        QVERIFY(couldCommit);
        QVERIFY(asked.contains(QLatin1String("transaction open")));
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);
        QCOMPARE(tableCount("tx_committed"), 1);
        runSql("DROP TABLE tx_committed");
    }

    void cancelKeepsTransaction()
    {
        m_tab->setTransactionPrompt(
            [](const QString &, bool) { return EditorPage::TransactionChoice::Cancel; });
        runSql("BEGIN");
        QVERIFY(!m_tab->maybeClose());
        QVERIFY(!m_tab->changeConnection(nullptr));
        QCOMPARE(m_tab->session(), m_session.get());
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);
        runSql("ROLLBACK");
    }

    void switchingRollsBack()
    {
        m_tab->setTransactionPrompt(
            [](const QString &, bool) { return EditorPage::TransactionChoice::RollBack; });
        runSql("BEGIN");
        runSql("CREATE TABLE tx_rolled_back (i int)");
        QVERIFY(m_tab->changeConnection(nullptr));
        QVERIFY(!m_tab->session());
        m_tab->setSession(m_session.get());
        QTRY_COMPARE(m_tab->connection()->state(), pg::Connection::State::Ready);
        QCOMPARE(tableCount("tx_rolled_back"), 0);
    }

    void failedTransactionCannotCommit()
    {
        std::optional<bool> couldCommit;
        m_tab->setTransactionPrompt([&](const QString &, bool canCommit) {
            couldCommit = canCommit;
            return EditorPage::TransactionChoice::Commit; // Not offered; refused.
        });
        runSql("BEGIN");
        runSql("SELECT 1/0");
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INERROR);
        QVERIFY(!m_tab->maybeClose());
        QCOMPARE(couldCommit, std::optional<bool>(false));
        runSql("ROLLBACK");
    }

    void failedCommitKeepsPage()
    {
        m_tab->setTransactionPrompt(
            [](const QString &, bool) { return EditorPage::TransactionChoice::Commit; });
        runSql("BEGIN");
        runSql("CREATE TEMP TABLE tx_deferred (i int UNIQUE DEFERRABLE INITIALLY DEFERRED)");
        runSql("INSERT INTO tx_deferred VALUES (1), (1)");
        QVERIFY(!m_tab->maybeClose()); // COMMIT fails on the deferred check.
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);
        QVERIFY(messages().contains(QLatin1String("duplicate key")));
    }

    void runningStatementIsStopped()
    {
        std::optional<bool> couldCommit;
        m_tab->setTransactionPrompt([&](const QString &, bool canCommit) {
            couldCommit = canCommit;
            return EditorPage::TransactionChoice::RollBack;
        });
        setText("SELECT pg_sleep(30)");
        m_tab->editor()->setModified(false);
        m_tab->run();
        QVERIFY(m_tab->isRunning());
        QVERIFY(m_tab->maybeClose());
        QCOMPARE(couldCommit, std::optional<bool>(false));
    }

    void columnWidths()
    {
        runSql("SELECT repeat('x', 400) AS a_very_long_column_name_for_a_narrow_value, "
               "1 AS n, to_jsonb(repeat('z', 400)) AS j");
        QCOMPARE(model()->rowCount(), 1);
        QTableView *table = m_tab->resultPanel()->results()->table();
        QHeaderView *header = table->horizontalHeader();
        QCOMPARE(model()->columnCount(), 3);
        for (int c = 0; c < model()->columnCount(); ++c) {
            // The header's text is never cut off, and no column hogs the view.
            const QString name = model()->headerData(c, Qt::Horizontal).toString();
            QVERIFY2(table->columnWidth(c) >= header->fontMetrics().horizontalAdvance(name),
                     qPrintable(QStringLiteral("column %1 is too narrow for its name").arg(c)));
            QVERIFY(table->columnWidth(c) <= 600);
        }
        // Long values are cut off at the content limit, and the long name
        // makes its own column wider than the number under it.
        QVERIFY(table->columnWidth(0) <= 400);
        QVERIFY(table->columnWidth(2) <= 400);
        QVERIFY(table->columnWidth(0) > table->columnWidth(1));
    }

    void viewModes()
    {
        runSql("SELECT i AS n, repeat('x', i) AS name FROM generate_series(1, 3) i");
        ResultView *view = m_tab->resultPanel()->results();
        QCOMPARE(view->viewMode(), ResultView::ViewMode::Grid);
        QVERIFY(view->table()->isVisibleTo(view));

        // Text: the same rows as an ASCII table, ready to copy out.
        view->setViewMode(ResultView::ViewMode::Text);
        QVERIFY(view->textView()->isVisibleTo(view));
        const QString text = view->textView()->text();
        QVERIFY(text.contains(QLatin1String("n | name")));
        QVERIFY(text.contains(QLatin1String("3 | xxx")));

        // Record: one row, its columns under each other.
        view->setViewMode(ResultView::ViewMode::Record);
        QVERIFY(view->recordView()->isVisibleTo(view));
        QAbstractItemModel *record = view->recordView()->model();
        QCOMPARE(record->rowCount(), 2);
        QCOMPARE(record->index(0, 0).data().toString(), QStringLiteral("n"));
        QCOMPARE(record->index(0, 1).data().toString(), QStringLiteral("1"));
        QCOMPARE(record->index(1, 1).data().toString(), QStringLiteral("x"));

        // It follows the grid's row, and the arrows move it.
        view->table()->setCurrentIndex(model()->index(2, 0));
        QCOMPARE(record->index(1, 1).data().toString(), QStringLiteral("xxx"));

        view->setViewMode(ResultView::ViewMode::Grid);
        QVERIFY(view->table()->isVisibleTo(view));

        // Alt+drag territory: a block of the text view is one column of the
        // table, so a single column can be copied out.
        view->setViewMode(ResultView::ViewMode::Text);
        ResultTextView *textView = view->textView();
        const QString rendered = textView->text();
        const qsizetype nameColumn = rendered.indexOf(QLatin1String("name"));
        QVERIFY(nameColumn > 0);
        const qsizetype lastLine = rendered.lastIndexOf(QLatin1Char('\n'), -2) + 1;
        textView->selectBlock(nameColumn, lastLine + nameColumn + 3);
        QCOMPARE(textView->selection(), QStringLiteral("nam\n---\nx\nxx\nxxx\n"));

        // A new query re-renders the text view rather than keeping the old.
        view->setViewMode(ResultView::ViewMode::Text);
        runSql("SELECT 'fresh' AS word");
        QVERIFY(view->textView()->text().contains(QLatin1String("fresh")));
        QVERIFY(!view->textView()->text().contains(QLatin1String("xxx")));
    }

    void editPaletteToggles()
    {
        ResultView *view = m_tab->resultPanel()->results();
        QToolButton *toggle = view->editToggle();

        // A query nothing can be written back to: no editing tools at all.
        runSql("SELECT 1 AS n");
        QVERIFY(!toggle->isEnabled());
        QVERIFY(!view->editBar()->isVisibleTo(view));

        runSql("CREATE TEMP TABLE palette (id int PRIMARY KEY, note text)");
        runSql("INSERT INTO palette VALUES (1, 'one')");
        runSql("SELECT * FROM palette");
        QTRY_VERIFY(model()->isEditable());
        QVERIFY(toggle->isEnabled());
        QVERIFY(!view->editBar()->isVisibleTo(view)); // Out of the way until asked for.

        toggle->click();
        QVERIFY(view->editBar()->isVisibleTo(view));
        toggle->click();
        QVERIFY(!view->editBar()->isVisibleTo(view));

        // An unsaved change brings it back: Save and Discard must be reachable.
        QVERIFY(model()->setData(model()->index(0, 1), QStringLiteral("two")));
        QVERIFY(toggle->isChecked());
        QVERIFY(view->editBar()->isVisibleTo(view));
        model()->discardChanges();

        // And it goes away again with a read-only result.
        runSql("SELECT 1 AS n");
        QVERIFY(!toggle->isEnabled());
        QVERIFY(!toggle->isChecked());
        QVERIFY(!view->editBar()->isVisibleTo(view));
    }

    void modeTabsSwitchViews()
    {
        runSql("SELECT 1 AS n");
        ResultView *view = m_tab->resultPanel()->results();
        QCOMPARE(view->modeTabs()->count(), 3);
        view->modeTabs()->setCurrentIndex(1);
        QCOMPARE(view->viewMode(), ResultView::ViewMode::Text);
        QVERIFY(view->textView()->isVisibleTo(view));
        view->setViewMode(ResultView::ViewMode::Grid);
        QCOMPARE(view->modeTabs()->currentIndex(), 0);
    }

    void exportsSelectedRows()
    {
        runSql("SELECT i AS n FROM generate_series(1, 5) i");
        ResultView *view = m_tab->resultPanel()->results();
        QCOMPARE(model()->rowCount(), 5);
        QVERIFY(view->allRowsFetched());

        // Rows 2 and 4, picked in the grid.
        QItemSelectionModel *selection = view->table()->selectionModel();
        selection->select(model()->index(1, 0),
                          QItemSelectionModel::Select | QItemSelectionModel::Rows);
        selection->select(model()->index(3, 0),
                          QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(view->selectedRows(), (std::vector<int> {1, 3}));

        catalog::ExportOptions options = view->exportOptions(catalog::ExportFormat::Csv);
        options.rows = view->selectedRows();
        QCOMPARE(catalog::exportRows(model()->rows(), options), QStringLiteral("n\n2\n4\n"));

        // Copy takes the selection too, and puts it on the clipboard.
        view->copyAs(catalog::ExportFormat::Csv);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("n\n2\n4\n"));

        // With nothing selected it is the whole result again.
        selection->clearSelection();
        view->copyAs(catalog::ExportFormat::Csv);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("n\n1\n2\n3\n4\n5\n"));
    }

    void exportsAllRowsBeyondTheLimit()
    {
        // The grid stops at the row limit, which libpq applies per chunk of
        // 1000 rows, so the query has to be bigger than one chunk.
        // Rows have to trickle in for the limit's cancel to land before the
        // query is over: a millisecond each, and chunks of 1000.
        m_tab->setRowLimit(2);
        runSql("SELECT i AS n FROM generate_series(1, 1500) i WHERE pg_sleep(0.001)::text = ''");
        ResultView *view = m_tab->resultPanel()->results();
        QVERIFY2(model()->rowCount() < 1500, qPrintable(QString::number(model()->rowCount())));
        QVERIFY(!view->allRowsFetched()); // So "all rows" runs it again.

        const QString path = m_dir.filePath(QStringLiteral("all.csv"));
        catalog::ExportOptions options = view->exportOptions(catalog::ExportFormat::Csv);
        Q_EMIT view->exportAllRequested(options, path);
        QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 20'000));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        QCOMPARE(lines.first(), QStringLiteral("n")); // One header, not one per chunk.
        QCOMPARE(lines.value(1), QStringLiteral("1"));
        QCOMPARE(lines.value(1500), QStringLiteral("1500"));
        QCOMPARE(lines.value(1501), QString()); // Nothing after the last row.
        QVERIFY2(messages().contains(QLatin1String("Exported 1500 rows")), qPrintable(messages()));

        // The grid itself is untouched by the export.
        QVERIFY(model()->rowCount() < 1500);
        m_tab->setRowLimit(1000);
    }

    void exportsRows()
    {
        runSql("SELECT i AS n, 'it''s' AS t FROM generate_series(1, 2) i");
        ResultView *view = m_tab->resultPanel()->results();
        const QString path = m_dir.filePath(QStringLiteral("rows.csv"));
        QString error;
        QVERIFY2(view->exportTo(view->exportOptions(catalog::ExportFormat::Csv), path, &error),
                 qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), QStringLiteral("n,t\n1,it's\n2,it's\n"));

        // SQL exports name the table the rows came from, when it is known.
        runSql("CREATE TEMP TABLE export_target (id int PRIMARY KEY, note text)");
        runSql("INSERT INTO export_target VALUES (1, 'one')");
        runSql("SELECT * FROM export_target");
        QTRY_VERIFY(model()->isEditable());
        const QString sql = catalog::exportRows(
            model()->rows(), view->exportOptions(catalog::ExportFormat::SqlInsert));
        QVERIFY2(sql.contains(QLatin1String("INSERT INTO ")), qPrintable(sql));
        QVERIFY2(sql.contains(QLatin1String("export_target (id, note) VALUES (1, 'one');")),
                 qPrintable(sql));
    }

    void transactionActions()
    {
        auto action = [&](const char *text) {
            for (QAction *a : m_tab->actions())
                if (a->text().remove(QLatin1Char('&')) == QLatin1String(text))
                    return a;
            return static_cast<QAction *>(nullptr);
        };
        QAction *begin = action("Begin");
        QAction *commit = action("Commit");
        QAction *rollback = action("Rollback");
        QVERIFY(begin && commit && rollback);
        auto enabled = [&] {
            return QList<bool> {begin->isEnabled(), commit->isEnabled(), rollback->isEnabled()};
        };
        auto wait = [&] { QVERIFY(QTest::qWaitFor([&] { return !m_tab->isRunning(); }, 10'000)); };
        QCOMPARE(enabled(), (QList<bool> {true, false, false}));
        QLabel *indicator = m_tab->transactionIndicator();
        QVERIFY(!indicator->isEnabled());
        QVERIFY(indicator->toolTip().contains(QLatin1String("Not in transaction")));
        QVERIFY(!indicator->pixmap().isNull());

        // Results on show stay there.
        runSql("SELECT 42");
        begin->trigger();
        QVERIFY(m_tab->isRunning());
        QCOMPARE(enabled(), (QList<bool> {false, false, false}));
        wait();
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_INTRANS);
        QCOMPARE(enabled(), (QList<bool> {false, true, true}));
        QCOMPARE(model()->index(0, 0).data().toString(), QStringLiteral("42"));
        QVERIFY(indicator->isEnabled());
        QVERIFY(indicator->toolTip().contains(QLatin1String("In transaction")));

        runSql("CREATE TABLE tx_toolbar (i int)");
        commit->trigger();
        wait();
        QCOMPARE(m_tab->connection()->transactionStatus(), PQTRANS_IDLE);
        QCOMPARE(enabled(), (QList<bool> {true, false, false}));
        QCOMPARE(tableCount("tx_toolbar"), 1);
        QVERIFY(messages().contains(QLatin1String("Committed.")));

        // Statements typed in the editor count the same.
        runSql("BEGIN");
        QCOMPARE(enabled(), (QList<bool> {false, true, true}));
        runSql("DROP TABLE tx_toolbar");
        runSql("SELECT 1/0");
        QCOMPARE(enabled(), (QList<bool> {false, false, true})); // Failed: only roll back.
        QVERIFY(indicator->isEnabled());
        QVERIFY(indicator->toolTip().contains(QLatin1String("failed")));
        rollback->trigger();
        wait();
        QCOMPARE(enabled(), (QList<bool> {true, false, false}));
        QCOMPARE(tableCount("tx_toolbar"), 1);
        runSql("DROP TABLE tx_toolbar");

        m_tab->setSession(nullptr);
        QCOMPARE(enabled(), (QList<bool> {false, false, false}));
        QVERIFY(!indicator->isEnabled());
    }

    void sessionCountsBusyEditors()
    {
        QCOMPARE(m_session->busyConnections(), 0);
        runSql("BEGIN");
        QCOMPARE(m_session->busyConnections(), 1);
        runSql("ROLLBACK");
        QCOMPARE(m_session->busyConnections(), 0);
        m_tab.reset();
        QCOMPARE(m_session->busyConnections(), 0); // A closed editor is forgotten.
    }

    void disconnectAsksAboutTransactions()
    {
        QSettings settings(m_dir.filePath(QStringLiteral("disconnect.ini")), QSettings::IniFormat);
        config::ConnectionProfile profile = m_server->profile;
        config::ProfileStore(settings).save(profile);
        config::PasswordStore passwords(settings, false);
        bool written = false;
        passwords.write(profile.id, config::PasswordStore::Secret::Postgres, m_server->password,
                        this, [&](bool) { written = true; });
        QTRY_VERIFY(written);
        ConnectionBrowser browser(settings, false);
        browser.connectProfile(profile.id);
        QTRY_COMPARE(browser.connectedSessions().size(), size_t(1));
        Session *session = browser.connectedSessions().front();

        EditorPage editor(&browser, QStringLiteral("Tx"));
        editor.setSession(session);
        QTRY_COMPARE(editor.connection()->state(), pg::Connection::State::Ready);
        editor.editor()->setText(QStringLiteral("BEGIN"));
        editor.run();
        QVERIFY(QTest::qWaitFor([&] { return !editor.isRunning(); }, 10'000));

        QString asked;
        bool answer = false;
        browser.setConfirm([&](const QString &question) {
            asked = question;
            return answer;
        });
        QVERIFY(!browser.disconnectProfile(profile.id));
        QVERIFY(asked.contains(QLatin1String("1 editor")));
        QCOMPARE(browser.connectedSessions().size(), size_t(1));
        QCOMPARE(editor.connection()->transactionStatus(), PQTRANS_INTRANS);

        answer = true;
        QVERIFY(browser.disconnectProfile(profile.id));
        QVERIFY(browser.connectedSessions().empty());
        QCOMPARE(editor.connection()->state(), pg::Connection::State::Disconnected);
    }

    void resultPageHasOwnConnection()
    {
        QStandardPaths::setTestModeEnabled(true);
        MainWindow w;
        ResultPage *page
            = w.showResult(m_session.get(), QStringLiteral("Slow"), "SELECT pg_sleep(30)");
        QVERIFY(page->connection());
        QVERIFY(page->connection() != m_session->runner()->connection());
        QTRY_VERIFY(page->results()->isRunning());

        // The shared connection is not held up meanwhile.
        bool done = false;
        m_session->runner()->run("SELECT 1", this, [&](const pg::QueryOutcome &) { done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5'000);

        // Stop cancels it.
        page->results()->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!page->results()->isRunning(), 5'000);

        // Disconnecting the session closes the page's connection.
        auto session = std::make_unique<Session>(m_server->profile,
                                                 Session::Credentials {m_server->password, {}});
        session->open();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        ResultPage *other = w.showResult(session.get(), QStringLiteral("One"), "SELECT 1");
        QTRY_COMPARE(other->results()->model()->rowCount(), 1);
        session->close();
        QCOMPARE(other->connection()->state(), pg::Connection::State::Disconnected);
    }

private:
    void runSql(const char *sql)
    {
        setText(sql);
        runAndWait();
        m_tab->editor()->setModified(false); // No "save the script?" in the way.
    }

    int tableCount(const char *name)
    {
        setText((QByteArray("SELECT count(*) FROM pg_class WHERE relname = '") + name + "'")
                    .constData());
        runAndWait();
        return model()->index(0, 0).data().toInt();
    }

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

    unsigned int extensionOid()
    {
        runSql("SELECT oid FROM pg_extension WHERE extname = 'plpgsql'");
        return model()->index(0, 0).data().toString().toUInt();
    }

    ResultModel *model() const { return m_tab->resultPanel()->results()->model(); }
    QString messages() const { return m_tab->resultPanel()->messages()->toPlainText(); }

    std::optional<TestServer> m_server;
    QTemporaryDir m_dir;
    std::optional<QSettings> m_settings;
    std::unique_ptr<ConnectionBrowser> m_browser;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<EditorPage> m_tab;
};

QTEST_MAIN(TestEditorPage)
#include "tst_editorpage.moc"
