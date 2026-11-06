// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserModel.h"
#include "CalloutLabel.h"
#include "ConnectionBrowser.h"
#include "SchemaDiffWizard.h"
#include "Session.h"
#include "TestServer.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <memory>

using namespace slonisko;

namespace {

const QByteArray TargetDatabase = "slonisko_wizard_target";
const QByteArray ReferenceDatabase = "slonisko_wizard_reference";

} // namespace

class TestSchemaDiffWizard : public QObject
{
    Q_OBJECT

    template<typename T>
    static T *child(const SchemaDiffWizard &wizard, const char *name)
    {
        return wizard.findChild<T *>(QString::fromUtf8(name));
    }

    // Runs sql as the test server's own profile, in database.
    bool admin(const QByteArray &sql, const QString &database = {})
    {
        config::ConnectionProfile profile = m_server->profile;
        if (!database.isEmpty())
            profile.database = database;
        Session session(profile, Session::Credentials {m_server->password, {}});
        session.open();
        if (!QTest::qWaitFor([&] { return session.state() == Session::State::Connected; }, 10'000))
            return false;
        std::optional<pg::QueryOutcome> outcome;
        session.runner()->run(sql, this, [&outcome](const pg::QueryOutcome &o) { outcome = o; });
        if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 30'000))
            return false;
        if (!outcome->ok())
            qWarning() << outcome->error;
        return outcome->ok();
    }

    // A browser with two saved connections to the test server, one per
    // database.
    std::unique_ptr<ConnectionBrowser> browser()
    {
        config::ProfileStore store(*m_settings);
        config::PasswordStore passwords(*m_settings, false);
        for (auto [id, database] : {std::pair(&m_targetId, TargetDatabase),
                                    std::pair(&m_referenceId, ReferenceDatabase)}) {
            config::ConnectionProfile profile = m_server->profile;
            profile.id = QUuid();
            profile.name = QString::fromUtf8(database);
            profile.database = QString::fromUtf8(database);
            store.save(profile);
            *id = profile.id;
            bool written = false;
            passwords.write(profile.id, config::PasswordStore::Secret::Postgres, m_server->password,
                            this, [&written](bool) { written = true; });
            [&] { QTRY_VERIFY(written); }();
        }
        return std::make_unique<ConnectionBrowser>(*m_settings, false);
    }

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        // The wizard's own settings, which are the application's.
        QSettings().remove(QStringLiteral("schemadiff"));
        m_dir.emplace();
        m_settings.emplace(m_dir->filePath(QStringLiteral("test.ini")), QSettings::IniFormat);
    }

    void cleanup()
    {
        m_settings.reset();
        m_dir.reset();
    }

    void withoutConnections()
    {
        ConnectionBrowser browser(*m_settings, false);
        SchemaDiffWizard wizard(&browser);
        wizard.show();
        QCOMPARE(wizard.currentId(), int(SchemaDiffWizard::ConnectionsPage));
        QCOMPARE(child<QComboBox>(wizard, "targetProfile")->count(), 0);
        QVERIFY(!wizard.currentPage()->isComplete());
        QVERIFY(!wizard.button(QWizard::NextButton)->isEnabled());

        // Reading the schemas fails without them, and says so as an error.
        wizard.setStartId(SchemaDiffWizard::SchemasPage);
        wizard.restart();
        auto *status = child<CalloutLabel>(wizard, "schemasStatus");
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Not connected."));
        QCOMPARE(status->tone(), CalloutLabel::Tone::Error);
    }

    void sameDatabase()
    {
        m_server = TestServer::find();
        if (!m_server)
            m_server = TestServer {};
        const std::unique_ptr<ConnectionBrowser> connections = browser();
        SchemaDiffWizard wizard(connections.get());
        wizard.show();
        // Nothing remembered, so nothing chosen.
        QVERIFY(wizard.targetProfile().isNull());
        QVERIFY(wizard.referenceProfile().isNull());
        QVERIFY(child<QComboBox>(wizard, "targetDatabase")->currentText().isEmpty());
        QVERIFY(child<QComboBox>(wizard, "referenceDatabase")->currentText().isEmpty());
        QVERIFY(!wizard.currentPage()->isComplete());

        // A connection chosen brings its own database.
        wizard.setTarget(m_targetId);
        wizard.setReference(m_referenceId);
        QCOMPARE(child<QComboBox>(wizard, "targetDatabase")->currentText(),
                 QString::fromUtf8(TargetDatabase));
        QVERIFY(wizard.currentPage()->isComplete());

        wizard.setReference(wizard.targetProfile());
        QCOMPARE(wizard.referenceDatabase(), wizard.targetDatabase());
        QVERIFY(!wizard.currentPage()->isComplete());
        auto *status = child<CalloutLabel>(wizard, "connectionsStatus");
        QVERIFY(status->text().contains(QLatin1String("same")));
        QCOMPARE(status->tone(), CalloutLabel::Tone::Error);

        // Another database of the same server is fine.
        wizard.setReference(wizard.targetProfile(), QStringLiteral("postgres"));
        QVERIFY(wizard.currentPage()->isComplete());
        QCOMPARE(status->tone(), CalloutLabel::Tone::Plain);
    }

    void remembered()
    {
        m_server = TestServer::find();
        if (!m_server)
            m_server = TestServer {};
        const std::unique_ptr<ConnectionBrowser> connections = browser();
        QSettings settings;
        settings.setValue(QStringLiteral("schemadiff/target"), m_targetId);
        settings.setValue(QStringLiteral("schemadiff/targetDatabase"), QStringLiteral("postgres"));
        settings.setValue(QStringLiteral("schemadiff/reference"), m_referenceId);
        {
            SchemaDiffWizard wizard(connections.get());
            QCOMPARE(wizard.targetProfile(), m_targetId);
            QCOMPARE(wizard.targetDatabase(), QStringLiteral("postgres"));
            QCOMPARE(wizard.referenceProfile(), m_referenceId);
            QCOMPARE(wizard.referenceDatabase(), QString::fromUtf8(ReferenceDatabase));
        }

        // A connection deleted since is not chosen.
        settings.setValue(QStringLiteral("schemadiff/reference"), QUuid::createUuid());
        SchemaDiffWizard wizard(connections.get());
        QCOMPARE(wizard.targetProfile(), m_targetId);
        QVERIFY(wizard.referenceProfile().isNull());
        QVERIFY(child<QComboBox>(wizard, "referenceDatabase")->currentText().isEmpty());
    }

    void compares()
    {
        m_server = TestServer::find();
        if (!m_server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        for (const QByteArray &database : {TargetDatabase, ReferenceDatabase}) {
            QVERIFY(admin("DROP DATABASE IF EXISTS " + database + " WITH (FORCE)"));
            QVERIFY(admin("CREATE DATABASE " + database));
        }
        QVERIFY(admin("CREATE SCHEMA wiz; CREATE TABLE wiz.kept (id int); "
                      "CREATE SCHEMA other; CREATE TABLE other.ignored (id int)",
                      QString::fromUtf8(TargetDatabase)));
        QVERIFY(admin("CREATE SCHEMA wiz; CREATE TABLE wiz.kept (id bigint); "
                      "CREATE TABLE wiz.fresh (x text)",
                      QString::fromUtf8(ReferenceDatabase)));

        {
            const std::unique_ptr<ConnectionBrowser> connections = browser();
            SchemaDiffWizard wizard(connections.get());
            wizard.show();
            wizard.setTarget(m_targetId);
            wizard.setReference(m_referenceId);
            QCOMPARE(child<QLabel>(wizard, "targetState")->text(), QStringLiteral("not connected"));

            // Next opens both connections, and goes on by itself.
            wizard.next();
            QCOMPARE(wizard.currentId(), int(SchemaDiffWizard::ConnectionsPage));
            QTRY_COMPARE_WITH_TIMEOUT(wizard.currentId(), int(SchemaDiffWizard::SchemasPage),
                                      15'000);
            QVERIFY(wizard.targetSession());
            QCOMPARE(wizard.targetSession()->state(), Session::State::Connected);

            // Of both databases.
            auto *schemas = child<QListWidget>(wizard, "schemas");
            QTRY_VERIFY(schemas->count() >= 3);
            QVERIFY(!schemas->findItems(QStringLiteral("other"), Qt::MatchExactly).isEmpty());
            child<QPushButton>(wizard, "noSchemas")->click();
            QVERIFY(!wizard.currentPage()->isComplete());
            schemas->findItems(QStringLiteral("wiz"), Qt::MatchExactly)
                .front()
                ->setCheckState(Qt::Checked);

            QSignalSpy compared(&wizard, &SchemaDiffWizard::compared);
            wizard.next();
            QCOMPARE(wizard.currentId(), int(SchemaDiffWizard::ResultPage));
            QTRY_COMPARE_WITH_TIMEOUT(compared.size(), 1, 30'000);
            QVERIFY2(compared.front().front().toBool(),
                     qPrintable(child<QLabel>(wizard, "status")->text()));
            QVERIFY(wizard.currentPage()->isComplete());

            QCOMPARE(wizard.diff().changes.size(), std::size_t(2));
            QCOMPARE(child<QTreeWidget>(wizard, "changes")->topLevelItemCount(), 2);
            QVERIFY(wizard.script().contains(QLatin1String("CREATE TABLE wiz.fresh")));
            QVERIFY(wizard.script().contains(
                QLatin1String("ALTER TABLE wiz.kept ALTER COLUMN id TYPE bigint")));
            QVERIFY(!wizard.script().contains(QLatin1String("ignored")));
            // Remembered from the last time, so set either way.
            child<QCheckBox>(wizard, "transaction")->setChecked(false);
            child<QCheckBox>(wizard, "transaction")->setChecked(true);
            QVERIFY(wizard.script().contains(QLatin1String("BEGIN;")));
            child<QCheckBox>(wizard, "transaction")->setChecked(false);
            QVERIFY(!wizard.script().contains(QLatin1String("BEGIN;")));

            QSignalSpy requested(&wizard, &SchemaDiffWizard::scriptRequested);
            child<QPushButton>(wizard, "openInEditor")->click();
            QCOMPARE(requested.size(), 1);
            QCOMPARE(requested.front().at(0).value<Session *>(), wizard.targetSession());
            QCOMPARE(requested.front().at(1).toString(), QString::fromUtf8(TargetDatabase));
            QCOMPARE(requested.front().at(2).toString(), wizard.script());
        }

        for (const QByteArray &database : {TargetDatabase, ReferenceDatabase})
            admin("DROP DATABASE IF EXISTS " + database + " WITH (FORCE)");
    }

private:
    std::optional<TestServer> m_server;
    std::optional<QTemporaryDir> m_dir;
    std::optional<QSettings> m_settings;
    QUuid m_targetId;
    QUuid m_referenceId;
};

QTEST_MAIN(TestSchemaDiffWizard)
#include "tst_schemadiffwizard.moc"
