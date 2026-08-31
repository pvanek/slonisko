// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"

#include <QCoreApplication>

namespace slonisko {

Session::Session(const config::ConnectionProfile &profile, const Credentials &credentials,
                 QObject *parent)
    : QObject(parent), m_profile(profile), m_credentials(credentials)
{ }

Session::~Session()
{
    closeConnections();
}

int Session::serverVersion() const
{
    const auto it = m_databases.find(m_profile.database);
    return it == m_databases.end() ? 0 : it->second.connection->serverVersion();
}

void Session::open()
{
    close();
    m_error.clear();
    setState(State::Connecting);

    if (!m_profile.ssh.enabled) {
        openMainConnection();
        return;
    }

    m_tunnel = new pg::SshTunnel(this);
    connect(m_tunnel, &pg::SshTunnel::stateChanged, this, [this](pg::SshTunnel::State s) {
        if (s == pg::SshTunnel::State::Ready && m_state == State::Connecting) {
            m_endpoint = {QStringLiteral("127.0.0.1"), m_tunnel->localPort()};
            openMainConnection();
        } else if (s == pg::SshTunnel::State::Failed) {
            fail(tr("SSH tunnel: %1").arg(m_tunnel->errorMessage()));
        }
    });

    const config::SshTunnelSettings &ssh = m_profile.ssh;
    pg::SshTunnel::Options options;
    options.host = ssh.host;
    options.port = ssh.port;
    options.user = ssh.user;
    if (ssh.auth == config::SshTunnelSettings::Auth::KeyFile)
        options.keyFile = ssh.keyFile;
    if (ssh.auth != config::SshTunnelSettings::Auth::Agent)
        options.secret = m_credentials.sshSecret;
    options.targetHost = m_profile.host;
    options.targetPort = m_profile.port;
    if (m_profile.connectTimeout > 0)
        options.timeoutMs = m_profile.connectTimeout * 1000;
    options.askpassProgram = QCoreApplication::applicationFilePath();
    m_tunnel->start(options);
}

void Session::close()
{
    closeConnections();
    setState(State::Disconnected);
}

pg::QueryRunner *Session::runner(const QString &database)
{
    if (m_state != State::Connected)
        return nullptr;
    const QString name = database.isEmpty() ? m_profile.database : database;
    const auto it = m_databases.find(name);
    return it != m_databases.end() ? it->second.runner : addDatabase(name).runner;
}

catalog::SnapshotPtr Session::snapshot(const QString &database)
{
    const QString name = database.isEmpty() ? m_profile.database : database;
    const auto it = m_snapshots.find(name);
    if (it != m_snapshots.end())
        return it->second;
    reloadSnapshot(name);
    return nullptr;
}

void Session::reloadSnapshot(const QString &database)
{
    const QString name = database.isEmpty() ? m_profile.database : database;
    pg::QueryRunner *r = runner(name);
    if (!r || m_loadingSnapshots.contains(name))
        return;
    m_loadingSnapshots.insert(name);
    const int version = serverVersion();
    r->run(catalog::snapshotQuery(), this, [this, name, version](const pg::QueryOutcome &outcome) {
        m_loadingSnapshots.erase(name);
        if (!outcome.ok())
            return; // Completion goes without; the next reload may work.
        if (catalog::SnapshotPtr loaded = catalog::parseSnapshot(outcome.results, version)) {
            m_snapshots[name] = std::move(loaded);
            Q_EMIT snapshotChanged(name);
        }
    });
}

void Session::openMainConnection()
{
    Database &main = addDatabase(m_profile.database);
    connect(main.connection, &pg::Connection::stateChanged, this, &Session::onMainStateChanged);
    // addDatabase() already opened it; a bad conninfo fails synchronously.
    onMainStateChanged(main.connection->state());
}

Session::Database &Session::addDatabase(const QString &name)
{
    Database &db = m_databases[name];
    db.connection = new pg::Connection(this);
    db.runner = new pg::QueryRunner(db.connection, this);
    db.connection->open(m_profile.conninfo(m_credentials.password, name, m_endpoint));
    return db;
}

void Session::onMainStateChanged(pg::Connection::State state)
{
    const auto it = m_databases.find(m_profile.database);
    if (it == m_databases.end())
        return;
    if (state == pg::Connection::State::Ready && m_state == State::Connecting)
        setState(State::Connected);
    else if (state == pg::Connection::State::Failed)
        fail(it->second.connection->errorMessage());
}

void Session::fail(const QString &message)
{
    m_error = message;
    closeConnections();
    setState(State::Failed);
}

void Session::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

void Session::closeConnections()
{
    // Runners fail their queued queries when their connection closes.
    std::map<QString, Database> databases;
    databases.swap(m_databases);
    for (auto &[name, db] : databases) {
        db.connection->disconnect(this);
        db.connection->close();
        db.runner->deleteLater();
        db.connection->deleteLater();
    }
    if (m_tunnel) {
        m_tunnel->disconnect(this);
        m_tunnel->stop();
        m_tunnel->deleteLater();
        m_tunnel = nullptr;
    }
    m_endpoint = {};
    m_snapshots.clear();
    m_loadingSnapshots.clear();
}

} // namespace slonisko
