// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/ConnectionProfile.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <libpq-fe.h>

#include <map>

using namespace slonisko::config;

namespace {

// Parses a conninfo the way libpq does.
std::map<QString, QString> parse(const QByteArray &conninfo)
{
    char *error = nullptr;
    PQconninfoOption *options = PQconninfoParse(conninfo.constData(), &error);
    if (!options) {
        const QString message = QString::fromUtf8(error);
        PQfreemem(error);
        qWarning("%s", qPrintable(message));
        return {};
    }
    std::map<QString, QString> out;
    for (const PQconninfoOption *o = options; o->keyword; ++o) {
        if (o->val)
            out[QString::fromUtf8(o->keyword)] = QString::fromUtf8(o->val);
    }
    PQconninfoFree(options);
    return out;
}

} // namespace

class TestConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir.emplace();
        m_settings.emplace(m_dir->filePath(QStringLiteral("test.ini")), QSettings::IniFormat);
    }

    void cleanup()
    {
        m_settings.reset();
        m_dir.reset();
    }

    void conninfo()
    {
        ConnectionProfile p;
        p.host = QStringLiteral("db.example.com");
        p.port = 6543;
        p.database = QStringLiteral("app");
        p.user = QStringLiteral("alice");
        p.sslMode = QStringLiteral("verify-full");
        p.sslRootCert = QStringLiteral("/etc/ssl/root ca.pem");
        p.connectTimeout = 7;
        p.extraParameters = QStringLiteral("options='-c search_path=app' target_session_attrs=any");

        const auto parsed = parse(p.conninfo(QStringLiteral("it's a \\ secret")));
        QCOMPARE(parsed.at(QStringLiteral("host")), QStringLiteral("db.example.com"));
        QCOMPARE(parsed.at(QStringLiteral("port")), QStringLiteral("6543"));
        QCOMPARE(parsed.at(QStringLiteral("dbname")), QStringLiteral("app"));
        QCOMPARE(parsed.at(QStringLiteral("user")), QStringLiteral("alice"));
        QCOMPARE(parsed.at(QStringLiteral("password")), QStringLiteral("it's a \\ secret"));
        QCOMPARE(parsed.at(QStringLiteral("sslmode")), QStringLiteral("verify-full"));
        QCOMPARE(parsed.at(QStringLiteral("sslrootcert")), QStringLiteral("/etc/ssl/root ca.pem"));
        QCOMPARE(parsed.at(QStringLiteral("connect_timeout")), QStringLiteral("7"));
        QCOMPARE(parsed.at(QStringLiteral("application_name")), QStringLiteral("Slonisko"));
        QCOMPARE(parsed.at(QStringLiteral("options")), QStringLiteral("-c search_path=app"));
        QCOMPARE(parsed.at(QStringLiteral("target_session_attrs")), QStringLiteral("any"));
        QVERIFY(!parsed.contains(QStringLiteral("hostaddr")));
        QVERIFY(!parsed.contains(QStringLiteral("sslcert")));
    }

    void conninfoOverrides()
    {
        ConnectionProfile p;
        p.host = QStringLiteral("db.internal");
        const auto parsed = parse(p.conninfo({}, QStringLiteral("other db"),
                                             Endpoint {QStringLiteral("127.0.0.1"), 40001}));
        // Connects to the tunnel, still verifies the certificate against db.internal.
        QCOMPARE(parsed.at(QStringLiteral("host")), QStringLiteral("db.internal"));
        QCOMPARE(parsed.at(QStringLiteral("hostaddr")), QStringLiteral("127.0.0.1"));
        QCOMPARE(parsed.at(QStringLiteral("port")), QStringLiteral("40001"));
        QCOMPARE(parsed.at(QStringLiteral("dbname")), QStringLiteral("other db"));
        QVERIFY(!parsed.contains(QStringLiteral("password")));
    }

    void conninfoUnicode()
    {
        ConnectionProfile p;
        p.user = QStringLiteral("uživatel");
        QCOMPARE(
            parse(p.conninfo(QStringLiteral("heslo žluťoučké"))).at(QStringLiteral("password")),
            QStringLiteral("heslo žluťoučké"));
        QCOMPARE(parse(p.conninfo({})).at(QStringLiteral("user")), QStringLiteral("uživatel"));
    }

    void displayName()
    {
        ConnectionProfile p;
        p.user = QStringLiteral("bob");
        p.host = QStringLiteral("h");
        QCOMPARE(p.displayName(), QStringLiteral("bob@h/postgres"));
        p.port = 5433;
        QCOMPARE(p.displayName(), QStringLiteral("bob@h:5433/postgres"));
        p.name = QStringLiteral("  Production  ");
        QCOMPARE(p.displayName(), QStringLiteral("Production"));
    }

    void profileRoundTrip()
    {
        ConnectionProfile p;
        p.name = QStringLiteral("Prod");
        p.color = QStringLiteral("#cc0000");
        p.host = QStringLiteral("prod");
        p.port = 5433;
        p.database = QStringLiteral("app");
        p.user = QStringLiteral("alice");
        p.passwordMode = PasswordMode::Ask;
        p.showAllDatabases = true;
        p.sslMode = QStringLiteral("require");
        p.sslCert = QStringLiteral("/c");
        p.sslKey = QStringLiteral("/k");
        p.sslRootCert = QStringLiteral("/r");
        p.applicationName = QStringLiteral("x");
        p.connectTimeout = 3;
        p.extraParameters = QStringLiteral("keepalives=1");
        p.ssh.enabled = true;
        p.ssh.host = QStringLiteral("bastion");
        p.ssh.port = 2222;
        p.ssh.user = QStringLiteral("jump");
        p.ssh.auth = SshTunnelSettings::Auth::KeyFile;
        p.ssh.keyFile = QStringLiteral("/home/a/.ssh/id_ed25519");
        p.ssh.passwordMode = PasswordMode::None;

        ProfileStore store(*m_settings);
        store.save(p);
        QVERIFY(!p.id.isNull());

        ConnectionProfile other;
        other.name = QStringLiteral("alpha");
        store.save(other);

        const auto loaded = store.load();
        QCOMPARE(loaded.size(), 2u);
        QCOMPARE(loaded[0].name, QStringLiteral("alpha")); // Sorted, case-insensitively.
        const ConnectionProfile &q = loaded[1];
        QCOMPARE(q.id, p.id);
        QCOMPARE(q.color, p.color);
        QCOMPARE(q.port, 5433);
        QCOMPARE(q.passwordMode, PasswordMode::Ask);
        QVERIFY(q.showAllDatabases);
        QCOMPARE(q.sslCert, p.sslCert);
        QCOMPARE(q.connectTimeout, 3);
        QCOMPARE(q.extraParameters, p.extraParameters);
        QVERIFY(q.ssh.enabled);
        QCOMPARE(q.ssh.port, 2222);
        QCOMPARE(q.ssh.auth, SshTunnelSettings::Auth::KeyFile);
        QCOMPARE(q.ssh.keyFile, p.ssh.keyFile);
        QCOMPARE(q.ssh.passwordMode, PasswordMode::None);
        QCOMPARE(q.conninfo(QStringLiteral("pw")), p.conninfo(QStringLiteral("pw")));

        store.remove(p.id);
        QCOMPARE(store.load().size(), 1u);
    }

    void defaultsForMissingKeys()
    {
        const QUuid id = QUuid::createUuid();
        m_settings->setValue(QStringLiteral("connections/") + id.toString(QUuid::WithoutBraces)
                                 + QStringLiteral("/name"),
                             QStringLiteral("minimal"));
        const auto loaded = ProfileStore(*m_settings).load();
        QCOMPARE(loaded.size(), 1u);
        const ConnectionProfile defaults;
        QCOMPARE(loaded[0].host, defaults.host);
        QCOMPARE(loaded[0].port, defaults.port);
        QCOMPARE(loaded[0].sslMode, defaults.sslMode);
        QCOMPARE(loaded[0].passwordMode, PasswordMode::Save);
        QVERIFY(!loaded[0].ssh.enabled);
    }

    void plainTextPasswordFallback()
    {
        ProfileStore profiles(*m_settings);
        ConnectionProfile p;
        profiles.save(p);

        PasswordStore passwords(*m_settings, false);
        QVERIFY(!passwords.usesWallet());

        std::optional<std::optional<QString>> read;
        passwords.read(p.id, PasswordStore::Secret::Postgres, this,
                       [&](std::optional<QString> pw) { read = pw; });
        QTRY_VERIFY(read.has_value());
        QVERIFY(!read->has_value()); // Nothing saved yet.

        std::optional<bool> inWallet;
        passwords.write(p.id, PasswordStore::Secret::Postgres, QStringLiteral("s3cret"), this,
                        [&](bool w) { inWallet = w; });
        passwords.write(p.id, PasswordStore::Secret::Ssh, QString(), this);
        QTRY_VERIFY(inWallet.has_value());
        QVERIFY(!*inWallet);

        read.reset();
        passwords.read(p.id, PasswordStore::Secret::Postgres, this,
                       [&](std::optional<QString> pw) { read = pw; });
        QTRY_VERIFY(read.has_value());
        QCOMPARE(read->value_or(QString()), QStringLiteral("s3cret"));

        // An empty password is still a saved password.
        read.reset();
        passwords.read(p.id, PasswordStore::Secret::Ssh, this,
                       [&](std::optional<QString> pw) { read = pw; });
        QTRY_VERIFY(read.has_value());
        QVERIFY(read->has_value());

        // Saving the profile again keeps it.
        profiles.save(p);
        read.reset();
        passwords.read(p.id, PasswordStore::Secret::Postgres, this,
                       [&](std::optional<QString> pw) { read = pw; });
        QTRY_VERIFY(read.has_value());
        QCOMPARE(read->value_or(QString()), QStringLiteral("s3cret"));

        // Stored with the profile, so removing the profile removes it.
        profiles.remove(p.id);
        read.reset();
        passwords.read(p.id, PasswordStore::Secret::Postgres, this,
                       [&](std::optional<QString> pw) { read = pw; });
        QTRY_VERIFY(read.has_value());
        QVERIFY(!read->has_value());
    }

    void callbackDroppedWithContext()
    {
        PasswordStore passwords(*m_settings, false);
        bool called = false;
        {
            QObject context;
            passwords.read(QUuid::createUuid(), PasswordStore::Secret::Postgres, &context,
                           [&](std::optional<QString>) { called = true; });
        }
        QTest::qWait(50);
        QVERIFY(!called);
    }

private:
    std::optional<QTemporaryDir> m_dir;
    std::optional<QSettings> m_settings;
};

QTEST_GUILESS_MAIN(TestConfig)
#include "tst_config.moc"
