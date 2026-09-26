// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Snapshot.h"
#include "config/ConnectionProfile.h"
#include "pg/Connection.h"
#include "pg/QueryRunner.h"
#include "pg/SshTunnel.h"

#include <QObject>
#include <QPointer>

#include <map>
#include <set>

namespace slonisko {

// A connected profile: the SSH tunnel if it uses one, a connection to its
// database, and connections to other databases of the same server, opened
// the first time something runs there.
class Session : public QObject
{
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected, Failed };
    Q_ENUM(State)

    struct Credentials
    {
        QString password;
        QString sshSecret; // SSH password or key passphrase.
    };

    Session(const config::ConnectionProfile &profile, const Credentials &credentials,
            QObject *parent = nullptr);
    ~Session() override;

    const config::ConnectionProfile &profile() const { return m_profile; }
    State state() const { return m_state; }
    QString errorMessage() const { return m_error; }
    int serverVersion() const;

    void open();
    void close();

    // A conninfo for a connection of one's own to a database (empty: the
    // profile's), through the session's SSH tunnel if it has one. Only valid
    // while connected.
    QByteArray conninfo(const QString &database = {}) const
    {
        return m_profile.conninfo(m_credentials.password, database, m_endpoint);
    }

    // Runs queries in a database; empty means the profile's own. Returns null
    // unless connected.
    pg::QueryRunner *runner(const QString &database = {});

    // Catalog data for completion in a database (empty: the profile's).
    // Loaded from the server on first use, so null at first; kept in memory
    // only, until reloadSnapshot() or disconnecting.
    catalog::SnapshotPtr snapshot(const QString &database = {});
    void reloadSnapshot(const QString &database = {});

    // Editors' connections of their own through this session. Disconnecting
    // the session ends them, rolling back what they have open.
    void attach(pg::Connection *connection);
    // How many of those are in a transaction or running a statement.
    int busyConnections() const;

Q_SIGNALS:
    void stateChanged(slonisko::Session::State state);
    void snapshotChanged(const QString &database);

private:
    struct Database
    {
        pg::Connection *connection = nullptr;
        pg::QueryRunner *runner = nullptr;
    };

    void openMainConnection();
    Database &addDatabase(const QString &name);
    void onMainStateChanged(pg::Connection::State state);
    void fail(const QString &message);
    void setState(State state);
    void closeConnections();

    config::ConnectionProfile m_profile;
    Credentials m_credentials;
    State m_state = State::Disconnected;
    QString m_error;
    pg::SshTunnel *m_tunnel = nullptr;
    config::Endpoint m_endpoint; // The tunnel's local end, if any.
    std::map<QString, Database> m_databases;
    std::map<QString, catalog::SnapshotPtr> m_snapshots;
    std::set<QString> m_loadingSnapshots;
    std::set<QString> m_staleSnapshots; // Changed again while loading.
    QList<QPointer<pg::Connection>> m_attached;
};

} // namespace slonisko
