// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/ProfileStore.h"

#include <QSettings>

#include <algorithm>

namespace slonisko::config {

namespace {

const QString Group = QStringLiteral("connections");

QString modeName(PasswordMode mode)
{
    switch (mode) {
    case PasswordMode::Save:
        return QStringLiteral("save");
    case PasswordMode::Ask:
        return QStringLiteral("ask");
    case PasswordMode::None:
        return QStringLiteral("none");
    }
    return {};
}

PasswordMode modeFromName(const QString &name, PasswordMode fallback)
{
    if (name == QLatin1String("save"))
        return PasswordMode::Save;
    if (name == QLatin1String("ask"))
        return PasswordMode::Ask;
    if (name == QLatin1String("none"))
        return PasswordMode::None;
    return fallback;
}

QString authName(SshTunnelSettings::Auth auth)
{
    switch (auth) {
    case SshTunnelSettings::Auth::Agent:
        return QStringLiteral("agent");
    case SshTunnelSettings::Auth::KeyFile:
        return QStringLiteral("key");
    case SshTunnelSettings::Auth::Password:
        return QStringLiteral("password");
    }
    return {};
}

SshTunnelSettings::Auth authFromName(const QString &name)
{
    if (name == QLatin1String("key"))
        return SshTunnelSettings::Auth::KeyFile;
    if (name == QLatin1String("password"))
        return SshTunnelSettings::Auth::Password;
    return SshTunnelSettings::Auth::Agent;
}

ConnectionProfile read(const QSettings &s, const QUuid &id)
{
    const ConnectionProfile d;
    ConnectionProfile p;
    p.id = id;
    p.name = s.value(QStringLiteral("name")).toString();
    p.color = s.value(QStringLiteral("color")).toString();
    p.host = s.value(QStringLiteral("host"), d.host).toString();
    p.port = s.value(QStringLiteral("port"), d.port).toInt();
    p.database = s.value(QStringLiteral("database"), d.database).toString();
    p.user = s.value(QStringLiteral("user")).toString();
    p.passwordMode
        = modeFromName(s.value(QStringLiteral("passwordMode")).toString(), d.passwordMode);
    p.showAllDatabases = s.value(QStringLiteral("showAllDatabases"), false).toBool();
    p.sslMode = s.value(QStringLiteral("sslMode"), d.sslMode).toString();
    p.sslRootCert = s.value(QStringLiteral("sslRootCert")).toString();
    p.sslCert = s.value(QStringLiteral("sslCert")).toString();
    p.sslKey = s.value(QStringLiteral("sslKey")).toString();
    p.applicationName = s.value(QStringLiteral("applicationName"), d.applicationName).toString();
    p.connectTimeout = s.value(QStringLiteral("connectTimeout"), d.connectTimeout).toInt();
    p.extraParameters = s.value(QStringLiteral("extraParameters")).toString();

    SshTunnelSettings &ssh = p.ssh;
    ssh.enabled = s.value(QStringLiteral("ssh/enabled"), false).toBool();
    ssh.host = s.value(QStringLiteral("ssh/host")).toString();
    ssh.port = s.value(QStringLiteral("ssh/port"), 22).toInt();
    ssh.user = s.value(QStringLiteral("ssh/user")).toString();
    ssh.auth = authFromName(s.value(QStringLiteral("ssh/auth")).toString());
    ssh.keyFile = s.value(QStringLiteral("ssh/keyFile")).toString();
    ssh.passwordMode
        = modeFromName(s.value(QStringLiteral("ssh/passwordMode")).toString(), PasswordMode::Save);
    return p;
}

void write(QSettings &s, const ConnectionProfile &p)
{
    s.setValue(QStringLiteral("name"), p.name);
    s.setValue(QStringLiteral("color"), p.color);
    s.setValue(QStringLiteral("host"), p.host);
    s.setValue(QStringLiteral("port"), p.port);
    s.setValue(QStringLiteral("database"), p.database);
    s.setValue(QStringLiteral("user"), p.user);
    s.setValue(QStringLiteral("passwordMode"), modeName(p.passwordMode));
    s.setValue(QStringLiteral("showAllDatabases"), p.showAllDatabases);
    s.setValue(QStringLiteral("sslMode"), p.sslMode);
    s.setValue(QStringLiteral("sslRootCert"), p.sslRootCert);
    s.setValue(QStringLiteral("sslCert"), p.sslCert);
    s.setValue(QStringLiteral("sslKey"), p.sslKey);
    s.setValue(QStringLiteral("applicationName"), p.applicationName);
    s.setValue(QStringLiteral("connectTimeout"), p.connectTimeout);
    s.setValue(QStringLiteral("extraParameters"), p.extraParameters);

    s.setValue(QStringLiteral("ssh/enabled"), p.ssh.enabled);
    s.setValue(QStringLiteral("ssh/host"), p.ssh.host);
    s.setValue(QStringLiteral("ssh/port"), p.ssh.port);
    s.setValue(QStringLiteral("ssh/user"), p.ssh.user);
    s.setValue(QStringLiteral("ssh/auth"), authName(p.ssh.auth));
    s.setValue(QStringLiteral("ssh/keyFile"), p.ssh.keyFile);
    s.setValue(QStringLiteral("ssh/passwordMode"), modeName(p.ssh.passwordMode));
}

} // namespace

std::vector<ConnectionProfile> ProfileStore::load() const
{
    std::vector<ConnectionProfile> out;
    m_settings.beginGroup(Group);
    for (const QString &key : m_settings.childGroups()) {
        const QUuid id(key);
        if (id.isNull())
            continue;
        m_settings.beginGroup(key);
        out.push_back(read(m_settings, id));
        m_settings.endGroup();
    }
    m_settings.endGroup();

    std::ranges::sort(out, [](const ConnectionProfile &a, const ConnectionProfile &b) {
        return a.displayName().compare(b.displayName(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

void ProfileStore::save(ConnectionProfile &profile)
{
    if (profile.id.isNull())
        profile.id = QUuid::createUuid();
    m_settings.beginGroup(Group + QLatin1Char('/') + profile.id.toString(QUuid::WithoutBraces));
    // Drop keys a previous version may have left, but keep the fallback
    // passwords PasswordStore keeps in the "passwords" subgroup.
    for (const QString &key : m_settings.childKeys())
        m_settings.remove(key);
    m_settings.remove(QStringLiteral("ssh"));
    write(m_settings, profile);
    m_settings.endGroup();
}

void ProfileStore::remove(const QUuid &id)
{
    m_settings.remove(Group + QLatin1Char('/') + id.toString(QUuid::WithoutBraces));
}

} // namespace slonisko::config
