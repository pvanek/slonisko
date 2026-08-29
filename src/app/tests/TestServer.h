// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/ConnectionProfile.h"

#include <QFile>

#include <libpq-fe.h>

#include <optional>

// The test server's conninfo as a profile and password, from
// SLONISKO_TEST_CONNINFO or the file the CTest fixture writes.
struct TestServer
{
    slonisko::config::ConnectionProfile profile;
    QString password;

    static std::optional<TestServer> find()
    {
        QByteArray conninfo = qgetenv("SLONISKO_TEST_CONNINFO");
        if (conninfo.isEmpty()) {
            QFile file(qEnvironmentVariable("SLONISKO_TEST_CONNINFO_FILE"));
            if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
                return std::nullopt;
            conninfo = file.readAll().trimmed();
        }
        PQconninfoOption *options = PQconninfoParse(conninfo.constData(), nullptr);
        if (!options)
            return std::nullopt;

        TestServer server;
        server.profile.id = QUuid::createUuid();
        server.profile.name = QStringLiteral("Test");
        server.profile.sslMode = QStringLiteral("disable");
        for (const PQconninfoOption *o = options; o->keyword; ++o) {
            if (!o->val)
                continue;
            const QByteArray key = o->keyword;
            const QString value = QString::fromUtf8(o->val);
            if (key == "host")
                server.profile.host = value;
            else if (key == "port")
                server.profile.port = value.toInt();
            else if (key == "user")
                server.profile.user = value;
            else if (key == "dbname")
                server.profile.database = value;
            else if (key == "password")
                server.password = value;
        }
        PQconninfoFree(options);
        return server;
    }
};
