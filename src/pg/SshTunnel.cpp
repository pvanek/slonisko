// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/SshTunnel.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>
#include <cstdio>

namespace slonisko::pg {

namespace {

const char *const AskpassFlag = "SLONISKO_ASKPASS";
const char *const AskpassSecret = "SLONISKO_ASKPASS_SECRET";

// A free port on the loopback interface. Another process could take it
// before ssh does; ExitOnForwardFailure turns that into an error.
int freeLocalPort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return 0;
    return server.serverPort();
}

} // namespace

SshTunnel::SshTunnel(QObject *parent) : QObject(parent)
{
    m_probeTimer.setInterval(100);
    connect(&m_probeTimer, &QTimer::timeout, this, &SshTunnel::probe);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this,
            [this] { fail(tr("Timed out opening the SSH tunnel")); });
}

SshTunnel::~SshTunnel()
{
    stop();
}

QStringList SshTunnel::arguments(const Options &o, int localPort)
{
    // IPv6 addresses need brackets in a forwarding specification.
    const QString target = o.targetHost.contains(QLatin1Char(':'))
        ? QLatin1Char('[') + o.targetHost + QLatin1Char(']')
        : o.targetHost;
    QStringList args {
        QStringLiteral("-N"), // Only forward, run no command.
        QStringLiteral("-T"),
        QStringLiteral("-o"),
        QStringLiteral("ExitOnForwardFailure=yes"),
        QStringLiteral("-o"),
        QStringLiteral("ServerAliveInterval=30"),
        QStringLiteral("-o"),
        QStringLiteral("ServerAliveCountMax=3"),
        QStringLiteral("-o"),
        QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-o"),
        QStringLiteral("ConnectTimeout=%1").arg(std::max(1, o.timeoutMs / 1000)),
        // Without a secret there is nobody to answer prompts: fail instead.
        QStringLiteral("-o"),
        o.secret.isEmpty() ? QStringLiteral("BatchMode=yes") : QStringLiteral("BatchMode=no"),
        QStringLiteral("-o"),
        QStringLiteral("NumberOfPasswordPrompts=1"),
        QStringLiteral("-L"),
        QStringLiteral("127.0.0.1:%1:%2:%3").arg(localPort).arg(target).arg(o.targetPort),
        QStringLiteral("-p"),
        QString::number(o.port),
    };
    if (!o.keyFile.isEmpty())
        args << QStringLiteral("-i") << o.keyFile << QStringLiteral("-o")
             << QStringLiteral("IdentitiesOnly=yes");
    if (!o.user.isEmpty())
        args << QStringLiteral("-l") << o.user;
    args << QStringLiteral("--") << o.host;
    return args;
}

bool SshTunnel::handleAskpass()
{
    if (!qEnvironmentVariableIsSet(AskpassFlag))
        return false;
    const QByteArray secret = qgetenv(AskpassSecret);
    std::fwrite(secret.constData(), 1, std::size_t(secret.size()), stdout);
    std::fputc('\n', stdout);
    return true;
}

void SshTunnel::start(const Options &options)
{
    stop();
    m_error.clear();
    m_stderr.clear();

    m_localPort = freeLocalPort();
    if (m_localPort == 0) {
        fail(tr("No free local port for the SSH tunnel"));
        return;
    }

    m_process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!options.secret.isEmpty()) {
        env.insert(QStringLiteral("SSH_ASKPASS"), options.askpassProgram);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
        env.insert(QString::fromLatin1(AskpassFlag), QStringLiteral("1"));
        env.insert(QString::fromLatin1(AskpassSecret), options.secret);
    }
    m_process->setProcessEnvironment(env);
    m_process->setStandardInputFile(QProcess::nullDevice());
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this] { m_stderr += m_process->readAllStandardError(); });
    connect(m_process, &QProcess::finished, this, &SshTunnel::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Could not run ssh: %1").arg(m_process->errorString()));
    });

    setState(State::Starting);
    m_timeout.start(options.timeoutMs);
    m_probeTimer.start();
    m_process->start(options.sshProgram, arguments(options, m_localPort));
}

void SshTunnel::stop()
{
    m_probeTimer.stop();
    m_timeout.stop();
    if (m_probe) {
        m_probe->abort();
        m_probe->deleteLater();
        m_probe = nullptr;
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_state != State::Failed)
        setState(State::Stopped);
}

// The tunnel is up once its local end accepts connections.
void SshTunnel::probe()
{
    if (m_state != State::Starting || m_probe)
        return;
    m_probe = new QTcpSocket(this);
    connect(m_probe, &QTcpSocket::connected, this, [this] {
        m_probe->abort();
        m_probe->deleteLater();
        m_probe = nullptr;
        m_probeTimer.stop();
        m_timeout.stop();
        setState(State::Ready);
    });
    connect(m_probe, &QTcpSocket::errorOccurred, this, [this] {
        m_probe->deleteLater();
        m_probe = nullptr;
    });
    m_probe->connectToHost(QHostAddress::LocalHost, quint16(m_localPort));
}

void SshTunnel::onProcessFinished()
{
    m_stderr += m_process->readAllStandardError();
    fail(m_state == State::Ready ? tr("The SSH tunnel closed: %1").arg(sshError()) : sshError());
}

void SshTunnel::fail(const QString &message)
{
    m_error = message;
    const State previous = m_state;
    m_state = State::Failed; // Keeps stop() from reporting Stopped.
    stop();
    m_state = previous;
    setState(State::Failed);
}

void SshTunnel::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

QString SshTunnel::sshError() const
{
    // ssh prints warnings (e.g. a newly added host key) before the error.
    QStringList lines;
    for (const QByteArray &line : m_stderr.split('\n')) {
        const QString text = QString::fromLocal8Bit(line).trimmed();
        if (!text.isEmpty() && !text.startsWith(QLatin1String("Warning: Permanently added")))
            lines << text;
    }
    return lines.isEmpty() ? tr("ssh exited unexpectedly") : lines.join(QLatin1Char('\n'));
}

} // namespace slonisko::pg
