// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class QProcess;
class QTcpSocket;

namespace slonisko::pg {

// Forwards a local port to a remote host through the system ssh client
// (OpenSSH): ssh -N -L 127.0.0.1:<local>:<target>:<port> host.
//
// Authentication uses the agent or a key file. A password or key passphrase
// is handed to ssh through SSH_ASKPASS: askpassProgram is run by ssh and must
// print SLONISKO_ASKPASS_SECRET from its environment (see handleAskpass()).
// Unknown host keys are accepted and remembered on first use; a changed key
// fails the connection.
class SshTunnel : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QString host;
        int port = 22;
        QString user;
        QString keyFile;
        QString secret; // Password or key passphrase; empty for none.
        QString targetHost;
        int targetPort = 5432;
        int timeoutMs = 30'000;
        QString sshProgram = QStringLiteral("ssh");
        QString askpassProgram; // Required when secret is set.
    };

    enum class State { Stopped, Starting, Ready, Failed };
    Q_ENUM(State)

    explicit SshTunnel(QObject *parent = nullptr);
    ~SshTunnel() override;

    void start(const Options &options);
    void stop();

    State state() const { return m_state; }
    QString errorMessage() const { return m_error; }
    // The local end of the tunnel on 127.0.0.1, valid once Ready.
    int localPort() const { return m_localPort; }

    static QStringList arguments(const Options &options, int localPort);

    // Call first thing in main(): if this process was started by ssh as the
    // askpass helper, prints the secret and returns true, and main() should
    // exit right away.
    static bool handleAskpass();

Q_SIGNALS:
    void stateChanged(slonisko::pg::SshTunnel::State state);

private:
    void probe();
    void onProcessFinished();
    void fail(const QString &message);
    void setState(State state);
    QString sshError() const;

    State m_state = State::Stopped;
    QString m_error;
    int m_localPort = 0;
    QProcess *m_process = nullptr;
    QTcpSocket *m_probe = nullptr;
    QByteArray m_stderr;
    QTimer m_probeTimer;
    QTimer m_timeout;
};

} // namespace slonisko::pg
