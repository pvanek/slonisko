// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserModel.h"
#include "ConnectionBrowser.h"
#include "DumpDialog.h"
#include "ImportDialog.h"
#include "RestoreDialog.h"
#include "Session.h"
#include "TestServer.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QWizardPage>

using namespace slonisko;
using NodeType = BrowserModel::NodeType;

class TestToolDialogs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_server = TestServer::find();
        if (!m_server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        m_session = std::make_unique<Session>(m_server->profile,
                                              Session::Credentials {m_server->password, {}});
        m_session->open();
        QTRY_COMPARE(m_session->state(), Session::State::Connected);
        QVERIFY(run("DROP SCHEMA IF EXISTS \"Tool Test\" CASCADE;"
                    "CREATE SCHEMA \"Tool Test\";"
                    "CREATE TABLE \"Tool Test\".t (id int PRIMARY KEY, note text);"
                    "INSERT INTO \"Tool Test\".t VALUES (1, 'one'), (2, 'two')"));
    }

    void cleanupTestCase()
    {
        if (m_session && m_session->state() == Session::State::Connected)
            run("DROP SCHEMA IF EXISTS \"Tool Test\" CASCADE");
    }

    void dumpCommandFollowsTheChoices()
    {
        DumpDialog dialog(m_session.get(), {}, QStringLiteral("Tool Test"), QStringLiteral("t"),
                          nullptr);
        auto *path = dialog.findChild<QLineEdit *>(QStringLiteral("path"));
        QVERIFY(path->text().endsWith(QLatin1String(".dump")));
        // No password on the command line: it goes into the environment.
        QVERIFY(dialog.commandText().contains(QLatin1String("pg_dump")));
        QVERIFY(dialog.commandText().contains(QLatin1String("--table=\"Tool Test\".\"t\"")));
        QVERIFY(!dialog.commandText().contains(QLatin1String("password")));
        QVERIFY(m_session->conninfo().contains("password") == !m_server->password.isEmpty());

        auto *format = dialog.findChild<QComboBox *>(QStringLiteral("format"));
        format->setCurrentIndex(format->findData(int(pg::DumpFormat::Plain)));
        QVERIFY(path->text().endsWith(QLatin1String(".sql")));
        QVERIFY(dialog.commandText().contains(QLatin1String("--format=plain")));
        QVERIFY(!dialog.findChild<QSpinBox *>(QStringLiteral("jobs"))->isEnabled());

        format->setCurrentIndex(format->findData(int(pg::DumpFormat::Directory)));
        QVERIFY(!path->text().endsWith(QLatin1String(".sql")));
        QVERIFY(dialog.findChild<QSpinBox *>(QStringLiteral("jobs"))->isEnabled());
        path->setText(m_dir.path()); // Not empty, as it holds this test's files.
        if (!QDir(m_dir.path()).isEmpty()) {
            QVERIFY(!dialog.canRun());
            QVERIFY(dialog.problemText().contains(QLatin1String("empty")));
        }
        path->clear();
        QVERIFY(!dialog.canRun());
        // Nor can it get past the first page.
        dialog.show();
        QVERIFY(!dialog.button(QWizard::NextButton)->isEnabled());
    }

    // A schema backed up with pg_dump, then restored with pg_restore after
    // it was changed, putting back what it was.
    void backsUpAndRestores()
    {
        DumpDialog dump(m_session.get(), {}, QStringLiteral("Tool Test"), {}, nullptr);
        if (!dump.canRun())
            QSKIP(qPrintable(QStringLiteral("Cannot run pg_dump: ") + dump.problemText()));
        const QString file = m_dir.filePath(QStringLiteral("tool test.dump"));
        dump.findChild<QLineEdit *>(QStringLiteral("path"))->setText(file);
        QSignalSpy dumped(&dump, &ToolDialog::toolFinished);
        QVERIFY(toLastPage(dump));
        dump.button(QWizard::FinishButton)->click();
        QVERIFY(dump.isRunning());
        // No going back to the choices while it runs.
        QVERIFY(!dump.button(QWizard::BackButton)->isEnabled());
        QVERIFY(!dump.button(QWizard::FinishButton)->isEnabled());
        QCOMPARE(dump.button(QWizard::CancelButton)->text(), QStringLiteral("&Stop"));
        QVERIFY(dumped.wait(60'000));
        QVERIFY2(dumped.first().at(0).toBool(), qPrintable(dump.logText()));
        QVERIFY(dump.statusText().contains(QLatin1String("Backed up")));
        QVERIFY(dump.logText().contains(QLatin1String("dumping contents of table")));
        QCOMPARE(pg::archiveKind(file), pg::ArchiveKind::Custom);
        // Done: it closes, or goes back to change something and run again.
        QVERIFY(!dump.button(QWizard::FinishButton)->isEnabled());
        QVERIFY(dump.button(QWizard::BackButton)->isEnabled());
        dump.back();
        dump.next();
        QVERIFY(dump.button(QWizard::FinishButton)->isEnabled());

        QVERIFY(run("UPDATE \"Tool Test\".t SET note = 'changed'"));

        RestoreDialog restore(m_session.get(), {}, nullptr);
        QStringList asked;
        restore.setConfirm([&](const QString &question) {
            asked << question;
            return true;
        });
        restore.setPath(file);
        QVERIFY(restore.findChild<QLabel *>(QStringLiteral("kind"))
                    ->text()
                    .contains(QLatin1String("Custom")));
        restore.findChild<QCheckBox *>(QStringLiteral("clean"))->setChecked(true);
        restore.findChild<QCheckBox *>(QStringLiteral("singleTransaction"))->setChecked(true);
        QVERIFY(restore.commandText().contains(QLatin1String("pg_restore")));
        QVERIFY(restore.commandText().contains(QLatin1String("--clean")));
        QSignalSpy restored(&restore, &ToolDialog::toolFinished);
        QVERIFY(toLastPage(restore));
        restore.button(QWizard::FinishButton)->click();
        QVERIFY(restored.wait(60'000));
        QVERIFY2(restored.first().at(0).toBool(), qPrintable(restore.logText()));
        QCOMPARE(asked.size(), 1);
        QVERIFY(asked.first().contains(QLatin1String("dropped")));
        QVERIFY(run("SELECT 1 FROM \"Tool Test\".t WHERE id = 1 AND note = 'one'", 1));

        // Saying no runs nothing.
        restore.setConfirm([](const QString &) { return false; });
        restore.back();
        restore.next();
        restore.button(QWizard::FinishButton)->click();
        QVERIFY(!restore.isRunning());
        QCOMPARE(restored.size(), 1);
    }

    // A plain script goes to psql, which has nothing to pick from it.
    void restoresAScriptWithPsql()
    {
        QFile script(m_dir.filePath(QStringLiteral("script.sql")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("INSERT INTO \"Tool Test\".t VALUES (3, 'three');\n");
        script.close();
        RestoreDialog restore(m_session.get(), {}, nullptr);
        restore.setConfirm([](const QString &) { return true; });
        restore.setPath(script.fileName());
        QVERIFY(restore.commandText().contains(QLatin1String("psql")));
        QVERIFY(restore.commandText().contains(QLatin1String("ON_ERROR_STOP=1")));
        QVERIFY(!restore.findChild<QCheckBox *>(QStringLiteral("clean"))->isEnabled());
        if (!restore.canRun())
            QSKIP(qPrintable(QStringLiteral("Cannot run psql: ") + restore.problemText()));
        // Nothing to pick from a script: its contents page is skipped.
        restore.show();
        restore.next();
        QCOMPARE(restore.currentPage()->objectName(), QStringLiteral("optionsPage"));
        QSignalSpy restored(&restore, &ToolDialog::toolFinished);
        QVERIFY(toLastPage(restore));
        restore.button(QWizard::FinishButton)->click();
        QVERIFY(restored.wait(60'000));
        QVERIFY2(restored.first().at(0).toBool(), qPrintable(restore.logText()));
        QVERIFY(run("SELECT 1 FROM \"Tool Test\".t WHERE id = 3", 1));

        restore.setPath(m_dir.filePath(QStringLiteral("missing.dump")));
        QVERIFY(!restore.canRun());
    }

#ifdef Q_OS_UNIX
    // A program chosen by hand is used, and an old one is warned about.
    void usesTheChosenProgram()
    {
        QDir(m_dir.path()).mkdir(QStringLiteral("old"));
        QFile fake(m_dir.filePath(QStringLiteral("old/pg_dump")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("#!/bin/sh\necho 'pg_dump (PostgreSQL) 9.6.24'\n");
        fake.close();
        fake.setPermissions(fake.permissions() | QFileDevice::ExeOwner);
        QSettings settings(m_dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
        settings.setValue(QLatin1String(ToolDialog::DirectoryKey),
                          m_dir.filePath(QStringLiteral("old")));
        DumpDialog dialog(m_session.get(), {}, {}, {}, &settings);
        QVERIFY(dialog.commandText().startsWith(fake.fileName()));
        QVERIFY2(dialog.problemText().contains(QLatin1String("older than the server")),
                 qPrintable(dialog.problemText()));
        QVERIFY(dialog.canRun()); // A warning only.
    }
#endif

    // The browser's menu opens them on what was clicked.
    void openFromTheBrowser()
    {
        QSettings settings(m_dir.filePath(QStringLiteral("browser.ini")), QSettings::IniFormat);
        config::ConnectionProfile profile = m_server->profile;
        profile.id = QUuid::createUuid();
        profile.passwordMode = config::PasswordMode::Save;
        config::ProfileStore(settings).save(profile);
        config::PasswordStore(settings, false)
            .write(profile.id, config::PasswordStore::Secret::Postgres, m_server->password, this);
        ConnectionBrowser browser(settings, false);
        browser.connectProfile(profile.id);
        QTRY_COMPARE(browser.connectedSessions().size(), std::size_t(1));
        BrowserModel *model = browser.model();

        const QModelIndex connection = model->profileIndex(profile.id);
        const QModelIndex db = child(model, connection, profile.database);
        const QModelIndex schema = child(model, child(model, db, QStringLiteral("Schemas")),
                                         QStringLiteral("Tool Test"));
        const QModelIndex table
            = child(model, child(model, schema, QStringLiteral("Tables")), QStringLiteral("t"));
        QVERIFY(table.isValid());

        DumpDialog *whole = browser.backUp(connection);
        QVERIFY(whole);
        QVERIFY(!whole->commandText().contains(QLatin1String("--schema")));
        DumpDialog *ofTable = browser.backUp(table);
        QVERIFY(ofTable);
        QVERIFY(ofTable->commandText().contains(QLatin1String("--table=\"Tool Test\".\"t\"")));
        QVERIFY(browser.restore(db));
        QVERIFY(!browser.restore(table));

        ImportDialog *into = browser.importCsv(table);
        QVERIFY(into);
        QCOMPARE(into->plan().schema, QStringLiteral("Tool Test"));
        QCOMPARE(into->plan().table, QStringLiteral("t"));
        QVERIFY(!into->plan().create);
        ImportDialog *intoNew = browser.importCsv(schema);
        QVERIFY(intoNew);
        QVERIFY(intoNew->plan().create);
        QVERIFY(!browser.importCsv(db));
        for (QDialog *d : browser.findChildren<QDialog *>())
            d->close();
    }

private:
    // Shown, and through its pages to the one that runs it.
    static bool toLastPage(QWizard &wizard)
    {
        if (!wizard.isVisible())
            wizard.show();
        for (int i = 0; i < 10 && wizard.nextId() != -1; ++i)
            wizard.next();
        return wizard.currentPage() == wizard.page(wizard.pageIds().constLast());
    }

    // Loaded, like a view expanding it, then the child of that name.
    static QModelIndex child(BrowserModel *model, const QModelIndex &parent, const QString &name)
    {
        if (model->canFetchMore(parent))
            model->fetchMore(parent);
        QModelIndex found;
        const bool there = QTest::qWaitFor(
            [&] {
                for (int row = 0; row < model->rowCount(parent); ++row) {
                    const QModelIndex c = model->index(row, 0, parent);
                    if (c.data().toString() == name) {
                        found = c;
                        return true;
                    }
                }
                return false;
            },
            10'000);
        if (!there)
            qWarning("No %s under %s", qPrintable(name), qPrintable(parent.data().toString()));
        return found;
    }

    bool run(const QByteArray &sql, int rows = -1)
    {
        std::optional<pg::QueryOutcome> outcome;
        QObject context;
        m_session->runner()->run(sql, &context, [&](const pg::QueryOutcome &o) { outcome = o; });
        if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 10'000) || !outcome->ok()) {
            qWarning() << (outcome ? outcome->error : QStringLiteral("timed out"));
            return false;
        }
        return rows < 0 || outcome->results.back().rowCount() == rows;
    }

    QTemporaryDir m_dir;
    std::optional<TestServer> m_server;
    std::unique_ptr<Session> m_session;
};

QTEST_MAIN(TestToolDialogs)
#include "tst_tooldialogs.moc"
