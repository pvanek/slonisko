// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionDialog.h"

#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;
using config::PasswordMode;
using config::SshTunnelSettings;

class TestConnectionDialog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roundTrip()
    {
        config::ConnectionProfile p;
        p.id = QUuid::createUuid();
        p.name = QStringLiteral("Prod");
        p.color = QStringLiteral("#cc0000");
        p.host = QStringLiteral("db");
        p.port = 6543;
        p.database = QStringLiteral("app");
        p.user = QStringLiteral("alice");
        p.passwordMode = PasswordMode::Ask;
        p.showAllDatabases = true;
        p.sslMode = QStringLiteral("verify-full");
        p.sslRootCert = QStringLiteral("/r");
        p.sslCert = QStringLiteral("/c");
        p.sslKey = QStringLiteral("/k");
        p.applicationName = QStringLiteral("x");
        p.connectTimeout = 0;
        p.extraParameters = QStringLiteral("keepalives=1");
        p.ssh.enabled = true;
        p.ssh.host = QStringLiteral("bastion");
        p.ssh.port = 2222;
        p.ssh.user = QStringLiteral("jump");
        p.ssh.auth = SshTunnelSettings::Auth::KeyFile;
        p.ssh.keyFile = QStringLiteral("/key");
        p.ssh.passwordMode = PasswordMode::Save;

        ConnectionDialog dialog(p, nullptr);
        const config::ConnectionProfile q = dialog.profile();
        QCOMPARE(q.id, p.id);
        QCOMPARE(q.color, p.color);
        QCOMPARE(q.conninfo(QStringLiteral("pw")), p.conninfo(QStringLiteral("pw")));
        QCOMPARE(q.passwordMode, p.passwordMode);
        QCOMPARE(q.showAllDatabases, true);
        QCOMPARE(q.ssh.enabled, true);
        QCOMPARE(q.ssh.host, p.ssh.host);
        QCOMPARE(q.ssh.port, p.ssh.port);
        QCOMPARE(q.ssh.user, p.ssh.user);
        QCOMPARE(q.ssh.auth, p.ssh.auth);
        QCOMPARE(q.ssh.keyFile, p.ssh.keyFile);
        QCOMPARE(q.ssh.passwordMode, PasswordMode::Save);
        QCOMPARE(q.connectTimeout, 0);
    }

    void sshSecretModes()
    {
        config::ConnectionProfile p;
        p.ssh.enabled = true;
        p.ssh.host = QStringLiteral("h");
        p.ssh.auth = SshTunnelSettings::Auth::Password;
        p.ssh.passwordMode = PasswordMode::Ask;
        QCOMPARE(ConnectionDialog(p, nullptr).profile().ssh.passwordMode, PasswordMode::Ask);

        // For a key, not saving a passphrase means it has none.
        p.ssh.auth = SshTunnelSettings::Auth::KeyFile;
        p.ssh.passwordMode = PasswordMode::None;
        QCOMPARE(ConnectionDialog(p, nullptr).profile().ssh.passwordMode, PasswordMode::None);
    }

    void passwordsOnlyWhenTyped()
    {
        config::ConnectionProfile p;
        p.id = QUuid::createUuid();
        ConnectionDialog dialog(p, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(!dialog.password().has_value());
        QVERIFY(!dialog.sshSecret().has_value());

        auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("password"));
        QVERIFY(password);
        QVERIFY(password->placeholderText().contains(QLatin1String("Saved")));
        QTest::keyClicks(password, QStringLiteral("new"));
        QCOMPARE(dialog.password().value_or(QString()), QStringLiteral("new"));
    }

    // The combo box items in ConnectionDialog.ui must stay in enum order.
    void comboItemsMatchEnums()
    {
        for (const PasswordMode mode :
             {PasswordMode::Save, PasswordMode::Ask, PasswordMode::None}) {
            config::ConnectionProfile p;
            p.passwordMode = mode;
            QCOMPARE(ConnectionDialog(p, nullptr).profile().passwordMode, mode);
        }
        for (const auto auth : {SshTunnelSettings::Auth::Agent, SshTunnelSettings::Auth::KeyFile,
                                SshTunnelSettings::Auth::Password}) {
            config::ConnectionProfile p;
            p.ssh.auth = auth;
            QCOMPARE(ConnectionDialog(p, nullptr).profile().ssh.auth, auth);
        }
        config::ConnectionProfile p;
        ConnectionDialog dialog(p, nullptr);
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("passwordMode"))->count(), 3);
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("sshAuth"))->count(), 3);
        for (const QString &mode : {QStringLiteral("disable"), QStringLiteral("verify-full")}) {
            p.sslMode = mode;
            QCOMPARE(ConnectionDialog(p, nullptr).profile().sslMode, mode);
        }
    }

    void colorIsNormalized()
    {
        config::ConnectionProfile p;
        p.color = QStringLiteral("red");
        QCOMPARE(ConnectionDialog(p, nullptr).profile().color, QStringLiteral("#ff0000"));
        p.color = QStringLiteral("not a color");
        QCOMPARE(ConnectionDialog(p, nullptr).profile().color, QString());
    }

    void validation()
    {
        config::ConnectionProfile p;
        p.host.clear();
        ConnectionDialog dialog(p, nullptr);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.testStatus().contains(QLatin1String("host")));
    }

    void testConnectionFails()
    {
        config::ConnectionProfile p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = 1; // Nothing listens there.
        p.passwordMode = PasswordMode::None;
        ConnectionDialog dialog(p, nullptr);
        dialog.testConnection();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.testStatus().startsWith(QLatin1String("Connecting")),
                                 10'000);
        QVERIFY2(dialog.testStatus().contains(QLatin1String("refused")),
                 qPrintable(dialog.testStatus()));
    }

    void serverVersions()
    {
        QCOMPARE(formatServerVersion(180001), QStringLiteral("18.1"));
        QCOMPARE(formatServerVersion(90624), QStringLiteral("9.6.24"));
    }
};

QTEST_MAIN(TestConnectionDialog)
#include "tst_connectiondialog.moc"
