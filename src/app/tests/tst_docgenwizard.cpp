// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DocGenWizard.h"
#include "Session.h"
#include "TestServer.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;

class TestDocGenWizard : public QObject
{
    Q_OBJECT

    template<typename T>
    static T *child(const DocGenWizard &wizard, const char *name)
    {
        return wizard.findChild<T *>(QString::fromUtf8(name));
    }

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void withoutConnection()
    {
        DocGenWizard wizard(nullptr, {});
        wizard.show();
        QCOMPARE(wizard.currentId(), int(DocGenWizard::SchemasPage));
        QCOMPARE(child<QLabel>(wizard, "schemasStatus")->text(), QStringLiteral("Not connected."));
        // No schema to choose, so no way on.
        QVERIFY(!wizard.currentPage()->isComplete());
        QVERIFY(!wizard.button(QWizard::NextButton)->isEnabled());
        QCOMPARE(child<QComboBox>(wizard, "format")->count(), 5);
    }

    // Short pages, so the wizard fits a small screen.
    void small()
    {
        DocGenWizard wizard(nullptr, {});
        wizard.show();
        QVERIFY2(wizard.minimumSizeHint().height() < 480,
                 qPrintable(QString::number(wizard.minimumSizeHint().height())));
    }

    void options()
    {
        DocGenWizard wizard(nullptr, {});
        wizard.show();
        auto *format = child<QComboBox>(wizard, "format");
        auto *antora = child<QLineEdit>(wizard, "antoraName");
        format->setCurrentIndex(format->findData(int(docgen::Format::Markdown)));
        QVERIFY(antora->isHidden());
        format->setCurrentIndex(format->findData(int(docgen::Format::Antora)));
        QVERIFY(!antora->isHidden());

        antora->setText(QStringLiteral("db"));
        child<QLineEdit>(wizard, "output")->setText(QStringLiteral("/tmp/out"));
        child<QLineEdit>(wizard, "title")->setText(QStringLiteral(" Shop "));
        child<QCheckBox>(wizard, "diagrams")->setChecked(false);
        child<QCheckBox>(wizard, "partitions")->setChecked(true);
        const docgen::Options options = wizard.options();
        QCOMPARE(options.output.format, docgen::Format::Antora);
        QCOMPARE(options.output.antoraName, QStringLiteral("db"));
        QCOMPARE(options.output.directory, QStringLiteral("/tmp/out"));
        QCOMPARE(options.pages.title, QStringLiteral("Shop"));
        QVERIFY(!options.pages.diagrams);
        QVERIFY(options.pages.partitions);
        QVERIFY(options.pages.symbolIndex);
    }

    void reportOptions()
    {
        DocGenWizard wizard(nullptr, {}, nullptr, DocGenWizard::Kind::QualityReport);
        wizard.show();
        QCOMPARE(wizard.kind(), DocGenWizard::Kind::QualityReport);
        // The documentation's choices make no sense for a report, and the
        // other way round.
        QVERIFY(child<QWidget>(wizard, "documentationOptions")->isHidden());
        QVERIFY(!child<QWidget>(wizard, "reportOptions")->isHidden());
        auto *format = child<QComboBox>(wizard, "format");
        format->setCurrentIndex(format->findData(int(docgen::Format::Antora)));
        QCOMPARE(child<QLineEdit>(wizard, "antoraName")->text(), QStringLiteral("report"));
        child<QLineEdit>(wizard, "output")->setText(QStringLiteral("/tmp/report"));
        child<QCheckBox>(wizard, "notices")->setChecked(false);
        const quality::Options options = wizard.reportOptions();
        QCOMPARE(options.output.format, docgen::Format::Antora);
        QCOMPARE(options.output.directory, QStringLiteral("/tmp/report"));
        QCOMPARE(options.output.antoraName, QStringLiteral("report"));
        QVERIFY(options.health);
        QVERIFY(!options.notices);

        DocGenWizard documentation(nullptr, {});
        documentation.show();
        QVERIFY(!child<QWidget>(documentation, "documentationOptions")->isHidden());
        QVERIFY(child<QWidget>(documentation, "reportOptions")->isHidden());
    }

    void generatesReport()
    {
        const std::optional<TestServer> server = TestServer::find();
        if (!server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        Session session(server->profile, Session::Credentials {server->password, {}});
        session.open();
        QTRY_COMPARE(session.state(), Session::State::Connected);
        std::optional<pg::QueryOutcome> setup;
        session.runner()->run("DROP SCHEMA IF EXISTS report_wizard CASCADE; "
                              "CREATE SCHEMA report_wizard; "
                              "CREATE TABLE report_wizard.thing (id int)",
                              this, [&setup](const pg::QueryOutcome &o) { setup = o; });
        QTRY_VERIFY(setup.has_value());
        QVERIFY2(setup->ok(), qPrintable(setup->error));

        DocGenWizard wizard(&session, {}, nullptr, DocGenWizard::Kind::QualityReport);
        wizard.show();
        QVERIFY(wizard.windowTitle().startsWith(QLatin1String("Quality Report of")));
        auto *schemas = child<QListWidget>(wizard, "schemas");
        QTRY_VERIFY(schemas->count() > 0);
        child<QPushButton>(wizard, "noSchemas")->click();
        schemas->findItems(QStringLiteral("report_wizard"), Qt::MatchExactly)
            .front()
            ->setCheckState(Qt::Checked);
        wizard.next();

        QTemporaryDir directory;
        child<QLineEdit>(wizard, "output")->setText(directory.path());
        child<QCheckBox>(wizard, "openWhenDone")->setChecked(false);
        wizard.next();
        QCOMPARE(wizard.currentId(), int(DocGenWizard::ContentsPage));
        QSignalSpy generated(&wizard, &DocGenWizard::generated);
        wizard.next();

        QTRY_COMPARE_WITH_TIMEOUT(generated.size(), 1, 60'000);
        const QString status = child<QLabel>(wizard, "status")->text();
        QVERIFY2(generated.front().front().toBool(), qPrintable(status));
        QVERIFY2(status.contains(QLatin1String(" warnings, ")), qPrintable(status));
        for (const char *page : {"index.html", "schema.html", "health.html", "statements.html"})
            QVERIFY2(QFile::exists(directory.filePath(QString::fromUtf8(page))), page);

        setup.reset();
        session.runner()->run("DROP SCHEMA report_wizard CASCADE", this,
                              [&setup](const pg::QueryOutcome &o) { setup = o; });
        QTRY_VERIFY(setup.has_value());
    }

    void generates()
    {
        const std::optional<TestServer> server = TestServer::find();
        if (!server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        Session session(server->profile, Session::Credentials {server->password, {}});
        session.open();
        QTRY_COMPARE(session.state(), Session::State::Connected);
        std::optional<pg::QueryOutcome> setup;
        session.runner()->run("DROP SCHEMA IF EXISTS docgen_wizard CASCADE; "
                              "CREATE SCHEMA docgen_wizard; "
                              "CREATE TABLE docgen_wizard.thing (id int PRIMARY KEY)",
                              this, [&setup](const pg::QueryOutcome &o) { setup = o; });
        QTRY_VERIFY(setup.has_value());
        QVERIFY2(setup->ok(), qPrintable(setup->error));

        // No database named: the profile's own.
        DocGenWizard wizard(&session, {});
        wizard.show();
        QCOMPARE(wizard.session(), &session);
        QCOMPARE(wizard.database(), server->profile.database);
        QVERIFY(wizard.windowTitle().contains(server->profile.database));

        // Schemas: only the one made here.
        auto *schemas = child<QListWidget>(wizard, "schemas");
        QTRY_VERIFY(schemas->count() > 0);
        QVERIFY(wizard.button(QWizard::NextButton)->isEnabled());
        child<QPushButton>(wizard, "noSchemas")->click();
        QVERIFY(!wizard.button(QWizard::NextButton)->isEnabled());
        const QList<QListWidgetItem *> mine
            = schemas->findItems(QStringLiteral("docgen_wizard"), Qt::MatchExactly);
        QCOMPARE(mine.size(), 1);
        mine.front()->setCheckState(Qt::Checked);
        QCOMPARE(wizard.options().schemas.include, QStringList {QStringLiteral("docgen_wizard")});
        wizard.next();

        // Format and folder: one with a file in it, so it asks, and no means
        // staying here.
        QCOMPARE(wizard.currentId(), int(DocGenWizard::OutputPage));
        QTemporaryDir directory;
        QFile stray(directory.filePath(QStringLiteral("stray.txt")));
        QVERIFY(stray.open(QIODevice::WriteOnly));
        stray.close();
        child<QLineEdit>(wizard, "output")->clear();
        QVERIFY(!wizard.button(QWizard::NextButton)->isEnabled());
        child<QLineEdit>(wizard, "output")->setText(directory.path());
        child<QCheckBox>(wizard, "openWhenDone")->setChecked(false);
        QString asked;
        wizard.setConfirm([&asked](const QString &question) {
            asked = question;
            return false;
        });
        wizard.next();
        QVERIFY(asked.contains(QLatin1String("already has files")));
        QCOMPARE(wizard.currentId(), int(DocGenWizard::OutputPage));
        wizard.setConfirm([](const QString &) { return true; });
        wizard.next();

        // Contents, whose Next generates.
        QCOMPARE(wizard.currentId(), int(DocGenWizard::ContentsPage));
        QCOMPARE(wizard.button(QWizard::NextButton)->text(), QStringLiteral("&Generate"));
        child<QCheckBox>(wizard, "diagrams")->setChecked(false);
        QSignalSpy generated(&wizard, &DocGenWizard::generated);
        wizard.next();

        QCOMPARE(wizard.currentId(), int(DocGenWizard::GeneratePage));
        QVERIFY(wizard.isRunning());
        QVERIFY(!wizard.button(QWizard::BackButton)->isVisible());
        QVERIFY(!wizard.button(QWizard::FinishButton)->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(generated.size(), 1, 60'000);
        QVERIFY2(generated.front().front().toBool(),
                 qPrintable(child<QLabel>(wizard, "status")->text()));
        QVERIFY(!wizard.isRunning());
        QVERIFY(wizard.lastResult().ok());
        QVERIFY(wizard.button(QWizard::FinishButton)->isEnabled());
        QVERIFY(wizard.button(QWizard::BackButton)->isVisible());
        QVERIFY(wizard.button(QWizard::BackButton)->isEnabled());
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("index.html"))));
        QVERIFY(
            QFile::exists(directory.filePath(QStringLiteral("docgen_wizard/tables/thing.html"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("stray.txt"))));
        QVERIFY(child<QLabel>(wizard, "status")->text().startsWith(QLatin1String("Wrote ")));

        setup.reset();
        session.runner()->run("DROP SCHEMA docgen_wizard CASCADE", this,
                              [&setup](const pg::QueryOutcome &o) { setup = o; });
        QTRY_VERIFY(setup.has_value());
    }
};

QTEST_MAIN(TestDocGenWizard)
#include "tst_docgenwizard.moc"
