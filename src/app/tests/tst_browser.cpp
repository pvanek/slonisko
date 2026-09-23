// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserModel.h"
#include "catalog/Details.h"
#include "ResultModel.h"
#include "ResultView.h"
#include "Session.h"
#include "TestServer.h"
#include "catalog/Monitoring.h"

#include <QAbstractItemModelTester>
#include <QTest>

#include <memory>

using namespace slonisko;
using NodeType = BrowserModel::NodeType;
using ObjectKind = catalog::ObjectKind;

namespace {

const QByteArray Setup = R"sql(
DROP SCHEMA IF EXISTS slonisko_browser CASCADE;
CREATE SCHEMA slonisko_browser;
CREATE TABLE slonisko_browser.t (id int PRIMARY KEY, name text NOT NULL);
CREATE VIEW slonisko_browser.v AS SELECT id FROM slonisko_browser.t;
)sql";

const QByteArray OtherDatabase = "slonisko_browser_other";

} // namespace

class TestBrowser : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_server = TestServer::find();
        if (!m_server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        QVERIFY(admin(Setup));
        QVERIFY(admin("DROP DATABASE IF EXISTS " + OtherDatabase + " WITH (FORCE)"));
        QVERIFY(admin("CREATE DATABASE " + OtherDatabase));
        QVERIFY(admin("CREATE TABLE only_there (x int)", QString::fromUtf8(OtherDatabase)));
    }

    void cleanupTestCase()
    {
        if (!m_server)
            return;
        admin("DROP SCHEMA IF EXISTS slonisko_browser CASCADE");
        admin("DROP DATABASE IF EXISTS " + OtherDatabase + " WITH (FORCE)");
    }

    void init()
    {
        m_model = std::make_unique<BrowserModel>();
        m_tester = std::make_unique<QAbstractItemModelTester>(
            m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    }

    void cleanup()
    {
        m_tester.reset();
        m_model.reset();
        m_session.reset();
    }

    void profilesOnly()
    {
        auto a = m_server->profile;
        a.name = QStringLiteral("b");
        auto b = m_server->profile;
        b.id = QUuid::createUuid();
        b.name = QStringLiteral("a");
        m_model->setProfiles({a, b});
        QCOMPARE(m_model->rowCount(), 2);
        QVERIFY(!m_model->hasChildren(m_model->index(0, 0))); // Not connected.

        auto c = m_server->profile;
        c.id = QUuid::createUuid();
        c.name = QStringLiteral("aa");
        m_model->addProfile(c); // Goes between "a" and "b"? Sorted among what is there.
        QCOMPARE(m_model->rowCount(), 3);

        c.name = QStringLiteral("renamed");
        m_model->updateProfile(c);
        QCOMPARE(m_model->profileIndex(c.id).data().toString(), QStringLiteral("renamed"));
        m_model->removeProfile(c.id);
        QCOMPARE(m_model->rowCount(), 2);
    }

    void browseToColumns()
    {
        const QModelIndex connection = connect(m_server->profile);
        QVERIFY(connection.isValid());
        QCOMPARE(childNames(connection),
                 (QStringList {m_server->profile.database, QStringLiteral("DBA Tools"),
                               QStringLiteral("System Info")}));
        QVERIFY(connection.data(Qt::FontRole).value<QFont>().bold());

        const QModelIndex db = child(connection, m_server->profile.database);
        QCOMPARE(childNames(expand(db)),
                 (QStringList {QStringLiteral("Schemas"), QStringLiteral("Extensions"),
                               QStringLiteral("Publications"), QStringLiteral("Event Triggers")}));

        const QModelIndex schemas = expand(child(db, QStringLiteral("Schemas")));
        QVERIFY(childNames(schemas).contains(QStringLiteral("slonisko_browser")));
        const QModelIndex catalog = child(schemas, QStringLiteral("pg_catalog"));
        QVERIFY(catalog.data(Qt::ForegroundRole).isValid()); // System objects are dimmed.

        const QModelIndex schema = expand(child(schemas, QStringLiteral("slonisko_browser")));
        QCOMPARE(childNames(expand(child(schema, QStringLiteral("Views")))),
                 QStringList {QStringLiteral("v")});
        const QModelIndex tables = expand(child(schema, QStringLiteral("Tables")));
        QCOMPARE(childNames(tables), QStringList {QStringLiteral("t")});

        const QModelIndex t = child(tables, QStringLiteral("t"));
        QCOMPARE(t.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>(),
                 catalog::ObjectKind::Table);
        QCOMPARE(t.data(BrowserModel::DatabaseRole).toString(), m_server->profile.database);
        const QModelIndex columns = expand(child(expand(t), QStringLiteral("Columns")));
        QCOMPARE(childNames(columns), (QStringList {QStringLiteral("id"), QStringLiteral("name")}));
        QCOMPARE(child(columns, QStringLiteral("name")).data(BrowserModel::DetailRole).toString(),
                 QStringLiteral("text not null"));
        QVERIFY(!m_model->hasChildren(child(columns, QStringLiteral("id"))));
    }

    void refreshReadsAgain()
    {
        const QModelIndex connection = connect(m_server->profile);
        const QModelIndex tables = expand(
            child(expand(child(expand(child(expand(child(connection, m_server->profile.database)),
                                            QStringLiteral("Schemas"))),
                               QStringLiteral("slonisko_browser"))),
                  QStringLiteral("Tables")));
        QCOMPARE(childNames(tables), QStringList {QStringLiteral("t")});

        QVERIFY(admin("CREATE TABLE slonisko_browser.added (x int)"));
        QCOMPARE(childNames(tables), QStringList {QStringLiteral("t")}); // Until refreshed.
        m_model->refresh(tables);
        QVERIFY(waitLoaded(tables));
        QCOMPARE(childNames(tables), (QStringList {QStringLiteral("added"), QStringLiteral("t")}));
        QVERIFY(admin("DROP TABLE slonisko_browser.added"));
    }

    void staleResultIgnored()
    {
        const QModelIndex connection = connect(m_server->profile);
        const QModelIndex schemas = child(expand(child(connection, m_server->profile.database)),
                                          QStringLiteral("Schemas"));
        m_model->fetchMore(schemas); // Starts loading...
        m_model->refresh(schemas); // ...and again, before the first load is done.
        QVERIFY(waitLoaded(schemas));
        QTest::qWait(200);
        const QStringList names = childNames(schemas);
        QCOMPARE(names.count(QStringLiteral("slonisko_browser")), 1);
    }

    void allDatabases()
    {
        auto profile = m_server->profile;
        profile.showAllDatabases = true;
        const QModelIndex connection = connect(profile);
        QTRY_VERIFY(childNames(connection).contains(QString::fromUtf8(OtherDatabase)));
        QVERIFY(childNames(connection).contains(profile.database));
        QVERIFY(!childNames(connection).contains(QStringLiteral("template0")));
        QVERIFY(childNames(connection).endsWith(QStringLiteral("System Info")));

        // Another database is browsed over its own connection.
        const QModelIndex other = child(connection, QString::fromUtf8(OtherDatabase));
        const QModelIndex tables
            = expand(child(expand(child(expand(child(expand(other), QStringLiteral("Schemas"))),
                                        QStringLiteral("public"))),
                           QStringLiteral("Tables")));
        QCOMPARE(childNames(tables), QStringList {QStringLiteral("only_there")});
    }

    void dbaTools()
    {
        const QModelIndex connection = connect(m_server->profile);
        const QModelIndex dba = expand(child(connection, QStringLiteral("DBA Tools")));
        const QStringList names = childNames(dba);
        QVERIFY(names.contains(QStringLiteral("Sessions")));
        QVERIFY(names.contains(QStringLiteral("Locks")));
        QVERIFY(names.endsWith(QStringLiteral("Tablespaces")));
        QVERIFY(childNames(expand(child(dba, QStringLiteral("Roles"))))
                    .contains(m_server->profile.user));

        const QModelIndex sessions = child(dba, QStringLiteral("Sessions"));
        QCOMPARE(sessions.data(BrowserModel::NodeTypeRole).value<NodeType>(), NodeType::Monitoring);
        const auto &query = catalog::monitoringQueries()[std::size_t(
            sessions.data(BrowserModel::MonitoringRole).toInt())];
        QCOMPARE(query.id, QStringLiteral("sessions"));

        ResultView view;
        view.run(m_session->runner(), query.title, query.sql(m_session->serverVersion()));
        QTRY_VERIFY(!view.isRunning());
        QVERIFY(view.model()->columnCount() > 5);
        QCOMPARE(view.model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("pid"));

        const QModelIndex info = expand(child(connection, QStringLiteral("System Info")));
        QVERIFY(childNames(info).contains(QStringLiteral("Server Overview")));
    }

    // Every kind the tree shows either opens a details page or does not,
    // and the ones that do carry what the page needs.
    void objectsWithDetails()
    {
        QVERIFY(catalog::hasDetails(ObjectKind::Table));
        QVERIFY(catalog::hasDetails(ObjectKind::Database));
        QVERIFY(catalog::hasDetails(ObjectKind::Schema));
        QVERIFY(catalog::hasDetails(ObjectKind::Sequence));
        QVERIFY(!catalog::hasDetails(ObjectKind::Column));

        const QModelIndex connection = connect(m_server->profile);
        const QModelIndex database = child(connection, m_server->profile.database);
        QVERIFY(database.isValid());
        QCOMPARE(database.data(BrowserModel::ObjectKindRole).value<ObjectKind>(),
                 ObjectKind::Database);

        const QModelIndex schemas = expand(child(expand(database), QStringLiteral("Schemas")));
        const QModelIndex schema = expand(child(schemas, QStringLiteral("slonisko_browser")));
        QCOMPARE(schema.data(BrowserModel::ObjectKindRole).value<ObjectKind>(), ObjectKind::Schema);

        const QModelIndex tables = expand(child(schema, QStringLiteral("Tables")));
        const QModelIndex table = child(tables, QStringLiteral("t"));
        QVERIFY(table.isValid());
        QCOMPARE(table.data(BrowserModel::ObjectKindRole).value<ObjectKind>(), ObjectKind::Table);
        QVERIFY(table.data(BrowserModel::OidRole).toUInt() > 0);
    }

    void disconnectClears()
    {
        const QModelIndex connection = connect(m_server->profile);
        expand(child(connection, m_server->profile.database));
        m_session->close();
        QCOMPARE(m_model->rowCount(connection), 0);
        QVERIFY(!connection.data(Qt::FontRole).value<QFont>().bold());

        m_session->open(); // Reconnects and shows everything again.
        QTRY_COMPARE(m_model->rowCount(connection), 3);

        m_model->setSession(m_server->profile.id, nullptr);
        QCOMPARE(m_model->rowCount(connection), 0);
    }

    void failedConnection()
    {
        m_model->setProfiles({m_server->profile});
        m_session = std::make_unique<Session>(m_server->profile,
                                              Session::Credentials {QStringLiteral("wrong"), {}});
        m_model->setSession(m_server->profile.id, m_session.get());
        m_session->open();
        const QModelIndex connection = m_model->profileIndex(m_server->profile.id);
        QCOMPARE(connection.data(BrowserModel::DetailRole).toString(),
                 QStringLiteral("connecting…"));
        QTRY_COMPARE(m_session->state(), Session::State::Failed);
        QCOMPARE(m_model->rowCount(connection), 0);
        QCOMPARE(connection.data(BrowserModel::DetailRole).toString(), QStringLiteral("failed"));
        QVERIFY(connection.data(Qt::ToolTipRole).toString().contains(QLatin1String("password")));
    }

    void sessionRunners()
    {
        m_session = std::make_unique<Session>(m_server->profile,
                                              Session::Credentials {m_server->password, {}});
        QVERIFY(!m_session->runner()); // Not connected yet.
        m_session->open();
        QTRY_COMPARE(m_session->state(), Session::State::Connected);
        QVERIFY(m_session->serverVersion() >= 140000);
        QCOMPARE(m_session->runner(), m_session->runner(m_server->profile.database));
        QVERIFY(m_session->runner(QString::fromUtf8(OtherDatabase)) != m_session->runner());

        // A database that does not exist fails its queries, not the session.
        std::optional<pg::QueryOutcome> outcome;
        m_session->runner(QStringLiteral("no_such_db"))
            ->run("SELECT 1", this, [&](const pg::QueryOutcome &o) { outcome = o; });
        QTRY_VERIFY(outcome.has_value());
        QVERIFY(outcome->error.contains(QLatin1String("does not exist")));
        QCOMPARE(m_session->state(), Session::State::Connected);
    }

private:
    // Runs sql as the test user, in database or the profile's.
    bool admin(const QByteArray &sql, const QString &database = {})
    {
        pg::Connection c;
        c.open(m_server->profile.conninfo(m_server->password, database));
        if (!QTest::qWaitFor([&] { return c.state() != pg::Connection::State::Connecting; },
                             10'000))
            return false;
        pg::QueryRunner runner(&c);
        std::optional<pg::QueryOutcome> outcome;
        runner.run(sql, this, [&](const pg::QueryOutcome &o) { outcome = o; });
        if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 30'000))
            return false;
        if (!outcome->ok())
            qWarning("%s", qPrintable(outcome->error));
        return outcome->ok();
    }

    QModelIndex connect(const config::ConnectionProfile &profile)
    {
        m_model->setProfiles({profile});
        m_session
            = std::make_unique<Session>(profile, Session::Credentials {m_server->password, {}});
        m_model->setSession(profile.id, m_session.get());
        m_session->open();
        if (!QTest::qWaitFor([&] { return m_session->state() == Session::State::Connected; },
                             10'000))
            return {};
        return m_model->profileIndex(profile.id);
    }

    // Loads the children, like a view does when the node is expanded.
    QModelIndex expand(const QModelIndex &index)
    {
        if (m_model->canFetchMore(index))
            m_model->fetchMore(index);
        waitLoaded(index);
        return index;
    }

    bool waitLoaded(const QModelIndex &index)
    {
        return QTest::qWaitFor(
            [&] {
                if (index.isValid() && m_model->hasChildren(index)
                    && !index.data(BrowserModel::LoadedRole).toBool()
                    && index.data(BrowserModel::NodeTypeRole).value<NodeType>()
                        != NodeType::Connection)
                    return false;
                for (int row = 0; row < m_model->rowCount(index); ++row) {
                    const QModelIndex c = m_model->index(row, 0, index);
                    if (c.data(BrowserModel::NodeTypeRole).value<NodeType>() == NodeType::Message
                        && !c.data(BrowserModel::ErrorRole).toBool())
                        return false;
                }
                return true;
            },
            10'000);
    }

    QModelIndex child(const QModelIndex &parent, const QString &name) const
    {
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            const QModelIndex c = m_model->index(row, 0, parent);
            if (c.data().toString() == name)
                return c;
        }
        qWarning("No %s under %s; have %s", qPrintable(name), qPrintable(parent.data().toString()),
                 qPrintable(childNames(parent).join(QStringLiteral(", "))));
        return {};
    }

    QStringList childNames(const QModelIndex &parent) const
    {
        QStringList out;
        for (int row = 0; row < m_model->rowCount(parent); ++row)
            out << m_model->index(row, 0, parent).data().toString();
        return out;
    }

    std::optional<TestServer> m_server;
    std::unique_ptr<BrowserModel> m_model;
    std::unique_ptr<QAbstractItemModelTester> m_tester;
    std::unique_ptr<Session> m_session;
};

QTEST_MAIN(TestBrowser)
#include "tst_browser.moc"
