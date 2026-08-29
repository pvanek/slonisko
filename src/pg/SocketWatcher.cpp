// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/SocketWatcher.h"

#include <QSocketNotifier>

namespace slonisko::pg {

void SocketWatcher::watch(int socket, bool read, bool write)
{
    if (socket != m_socket) {
        stop();
        m_socket = socket;
        m_readNotifier = new QSocketNotifier(socket, QSocketNotifier::Read, this);
        m_writeNotifier = new QSocketNotifier(socket, QSocketNotifier::Write, this);
        connect(m_readNotifier, &QSocketNotifier::activated, this, &SocketWatcher::activated);
        connect(m_writeNotifier, &QSocketNotifier::activated, this, &SocketWatcher::activated);
    }
    m_readNotifier->setEnabled(read);
    m_writeNotifier->setEnabled(write);
}

void SocketWatcher::stop()
{
    // May run inside a notifier's own activated() signal, hence deleteLater().
    for (QSocketNotifier *n : {m_readNotifier, m_writeNotifier}) {
        if (n) {
            n->setEnabled(false);
            n->deleteLater();
        }
    }
    m_readNotifier = nullptr;
    m_writeNotifier = nullptr;
    m_socket = -1;
}

} // namespace slonisko::pg
