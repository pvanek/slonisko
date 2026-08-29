// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/ConnectionProfile.h"

namespace slonisko::config {

QString ConnectionProfile::displayName() const
{
    if (!name.trimmed().isEmpty())
        return name.trimmed();
    QString out = user.isEmpty() ? host : user + QLatin1Char('@') + host;
    if (port != 5432)
        out += QLatin1Char(':') + QString::number(port);
    return out + QLatin1Char('/') + database;
}

QByteArray ConnectionProfile::conninfo(const QString &password, const QString &databaseName,
                                       const Endpoint &via) const
{
    QByteArray out;
    auto add = [&](const char *keyword, const QString &value) {
        if (value.isEmpty())
            return;
        if (!out.isEmpty())
            out += ' ';
        out += keyword;
        out += '=';
        out += quoteConninfoValue(value);
    };

    add("host", host);
    if (!via.host.isEmpty()) {
        // Connect to the tunnel but keep host for SSL certificate checks.
        add("hostaddr", via.host);
        add("port", QString::number(via.port));
    } else {
        add("port", QString::number(port));
    }
    add("dbname", databaseName.isEmpty() ? database : databaseName);
    add("user", user);
    add("password", password);
    add("sslmode", sslMode);
    add("sslrootcert", sslRootCert);
    add("sslcert", sslCert);
    add("sslkey", sslKey);
    add("application_name", applicationName);
    add("connect_timeout", QString::number(connectTimeout));

    const QString extra = extraParameters.trimmed();
    if (!extra.isEmpty())
        out += ' ' + extra.toUtf8();
    return out;
}

QByteArray quoteConninfoValue(const QString &value)
{
    QByteArray out = "'";
    for (const char c : value.toUtf8()) {
        if (c == '\'' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '\'';
}

} // namespace slonisko::config
