// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

namespace slonisko::config {

// How a password is obtained when connecting.
enum class PasswordMode {
    Save, // Stored in the system wallet (or the settings file as a fallback).
    Ask, // Asked for on every connect, never stored.
    None, // Not needed, or supplied by libpq itself (.pgpass, trust, peer, GSS).
};

struct SshTunnelSettings
{
    enum class Auth {
        Agent, // ssh-agent or ssh's default keys.
        KeyFile, // A private key, with an optional saved passphrase.
        Password,
    };

    bool enabled = false;
    QString host;
    int port = 22;
    QString user;
    Auth auth = Auth::Agent;
    QString keyFile;
    // For Auth::Password: Save or Ask. For Auth::KeyFile: Save stores a
    // passphrase, anything else means the key has none.
    PasswordMode passwordMode = PasswordMode::Save;
};

// Where to connect to instead of the profile's host and port, e.g. the local
// end of an SSH tunnel.
struct Endpoint
{
    QString host;
    int port = 0;
};

struct ConnectionProfile
{
    QUuid id;
    QString name;
    QString color; // #rrggbb, or empty for none.

    QString host = QStringLiteral("localhost");
    int port = 5432;
    QString database = QStringLiteral("postgres");
    QString user;
    PasswordMode passwordMode = PasswordMode::Save;
    // List all databases of the server in the browser, not just this one.
    bool showAllDatabases = false;

    QString sslMode = QStringLiteral("prefer");
    QString sslRootCert;
    QString sslCert;
    QString sslKey;

    QString applicationName = QStringLiteral("Slonisko");
    int connectTimeout = 10; // Seconds, 0 for none.
    // Further libpq keyword=value pairs, appended to the conninfo as they are.
    QString extraParameters;

    SshTunnelSettings ssh;

    // The name, or user@host:port/database when there is none.
    QString displayName() const;

    // A libpq conninfo string for this profile. databaseName overrides the
    // profile's database; via, if set, is where to actually connect (the
    // profile's host is still used to verify SSL certificates).
    QByteArray conninfo(const QString &password, const QString &databaseName = {},
                        const Endpoint &via = {}) const;
};

// Quotes a value for a libpq conninfo string.
QByteArray quoteConninfoValue(const QString &value);

} // namespace slonisko::config
