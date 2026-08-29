// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QSocketNotifier;

namespace slonisko::pg {

// Watches one socket for read and write readiness in the Qt event loop.
class SocketWatcher : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // The socket can change between calls: libpq may move to a new one while
    // connecting (multi-host conninfo, SSL/GSS retry).
    void watch(int socket, bool read, bool write);
    void stop();

Q_SIGNALS:
    void activated();

private:
    int m_socket = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
};

} // namespace slonisko::pg
