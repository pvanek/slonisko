// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/SshTunnel.h"

#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTest>

using slonisko::pg::SshTunnel;
using State = SshTunnel::State;

class TestSshTunnel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void arguments()
    {
        SshTunnel::Options o;
        o.host = QStringLiteral("bastion.example.com");
        o.port = 2222;
        o.user = QStringLiteral("jump");
        o.targetHost = QStringLiteral("db.internal");
        o.targetPort = 5433;
        o.timeoutMs = 7'000;

        const QStringList args = SshTunnel::arguments(o, 40000);
        QVERIFY(args.contains(QStringLiteral("-N")));
        QVERIFY(args.contains(QStringLiteral("ExitOnForwardFailure=yes")));
        QVERIFY(args.contains(QStringLiteral("BatchMode=yes"))); // No secret: never prompt.
        QVERIFY(args.contains(QStringLiteral("ConnectTimeout=7")));
        QCOMPARE(args[args.indexOf(QStringLiteral("-L")) + 1],
                 QStringLiteral("127.0.0.1:40000:db.internal:5433"));
        QCOMPARE(args[args.indexOf(QStringLiteral("-p")) + 1], QStringLiteral("2222"));
        QCOMPARE(args[args.indexOf(QStringLiteral("-l")) + 1], QStringLiteral("jump"));
        QVERIFY(!args.contains(QStringLiteral("-i")));
        QCOMPARE(args.last(), QStringLiteral("bastion.example.com"));
        QCOMPARE(args[args.size() - 2],
                 QStringLiteral("--")); // A host like "-oProxyCommand" is a host.
    }

    void argumentsWithKeyAndSecret()
    {
        SshTunnel::Options o;
        o.host = QStringLiteral("h");
        o.keyFile = QStringLiteral("/home/me/.ssh/id_ed25519");
        o.secret = QStringLiteral("passphrase");
        o.targetHost = QStringLiteral("::1");

        const QStringList args = SshTunnel::arguments(o, 1234);
        QCOMPARE(args[args.indexOf(QStringLiteral("-i")) + 1], o.keyFile);
        QVERIFY(args.contains(QStringLiteral("IdentitiesOnly=yes")));
        QVERIFY(args.contains(QStringLiteral("BatchMode=no")));
        QCOMPARE(args[args.indexOf(QStringLiteral("-L")) + 1],
                 QStringLiteral("127.0.0.1:1234:[::1]:5432"));
        QVERIFY(!args.contains(QStringLiteral("-l")));
    }

    void askpassPrintsSecret()
    {
        QProcess self;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SLONISKO_ASKPASS"), QStringLiteral("1"));
        env.insert(QStringLiteral("SLONISKO_ASKPASS_SECRET"), QStringLiteral("pa ss'wörd"));
        self.setProcessEnvironment(env);
        self.start(QCoreApplication::applicationFilePath(), {QStringLiteral("Password:")});
        QVERIFY(self.waitForFinished(10'000));
        QCOMPARE(self.exitCode(), 0);
        QCOMPARE(QString::fromLocal8Bit(self.readAllStandardOutput()),
                 QStringLiteral("pa ss'wörd\n"));
    }

    void missingSshProgram()
    {
        SshTunnel tunnel;
        SshTunnel::Options o;
        o.host = QStringLiteral("127.0.0.1");
        o.targetHost = QStringLiteral("127.0.0.1");
        o.sshProgram = QStringLiteral("/nonexistent/ssh");
        tunnel.start(o);
        QTRY_COMPARE(tunnel.state(), State::Failed);
        QVERIFY2(tunnel.errorMessage().contains(QLatin1String("Could not run ssh")),
                 qPrintable(tunnel.errorMessage()));
    }

    void connectionRefused()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ssh")).isEmpty())
            QSKIP("ssh is not installed");

        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost);
        const int port = closed.serverPort();
        closed.close();

        SshTunnel tunnel;
        QSignalSpy states(&tunnel, &SshTunnel::stateChanged);
        SshTunnel::Options o;
        o.host = QStringLiteral("127.0.0.1");
        o.port = port;
        o.targetHost = QStringLiteral("127.0.0.1");
        tunnel.start(o);
        QCOMPARE(tunnel.state(), State::Starting);
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), State::Failed, 10'000);
        QVERIFY2(tunnel.errorMessage().contains(QLatin1String("refused")),
                 qPrintable(tunnel.errorMessage()));
        QCOMPARE(states.size(), 2); // Starting, Failed.
    }

    void timeout()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ssh")).isEmpty())
            QSKIP("ssh is not installed");

        // Accepts the TCP connection and never sends an SSH banner.
        QTcpServer silent;
        silent.listen(QHostAddress::LocalHost);

        SshTunnel tunnel;
        SshTunnel::Options o;
        o.host = QStringLiteral("127.0.0.1");
        o.port = silent.serverPort();
        o.targetHost = QStringLiteral("127.0.0.1");
        o.timeoutMs = 1'000;
        tunnel.start(o);
        QTRY_COMPARE_WITH_TIMEOUT(tunnel.state(), State::Failed, 10'000);
        QVERIFY(!tunnel.errorMessage().isEmpty());
    }

    void stopWhileStarting()
    {
        SshTunnel tunnel;
        SshTunnel::Options o;
        o.host = QStringLiteral("127.0.0.1");
        o.targetHost = QStringLiteral("127.0.0.1");
        o.sshProgram = QStringLiteral("sleep"); // Stands in for an ssh that hangs.
        tunnel.start(o);
        QCOMPARE(tunnel.state(), State::Starting);
        tunnel.stop();
        QCOMPARE(tunnel.state(), State::Stopped);
        QTest::qWait(200);
        QCOMPARE(tunnel.state(), State::Stopped);
    }
};

// The test binary doubles as the askpass helper, like the application does.
int main(int argc, char *argv[])
{
    if (SshTunnel::handleAskpass())
        return 0;
    QCoreApplication app(argc, argv);
    TestSshTunnel test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_sshtunnel.moc"
