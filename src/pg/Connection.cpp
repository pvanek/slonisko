// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/Connection.h"

#include <QSocketNotifier>

namespace slonisko::pg {

Connection::Connection(QObject *parent) : QObject(parent) { }

Connection::~Connection() = default;

void Connection::open(const QByteArray &conninfo)
{
    close();
    m_error.clear();

    m_conn.reset(PQconnectStart(conninfo.constData()));
    if (!m_conn) {
        fail(tr("Could not allocate connection"));
        return;
    }
    if (PQstatus(m_conn.get()) == CONNECTION_BAD) {
        fail(QString::fromUtf8(PQerrorMessage(m_conn.get())).trimmed());
        return;
    }

    setState(State::Connecting);
    // libpq: after PQconnectStart, behave as if PQconnectPoll returned PGRES_POLLING_WRITING.
    watchSocket(false, true);
}

void Connection::close()
{
    dropNotifiers();
    m_conn.reset();
    setState(State::Disconnected);
}

int Connection::serverVersion() const
{
    return m_conn ? PQserverVersion(m_conn.get()) : 0;
}

void Connection::pollConnect()
{
    if (m_state != State::Connecting)
        return;

    switch (PQconnectPoll(m_conn.get())) {
    case PGRES_POLLING_READING:
        watchSocket(true, false);
        break;
    case PGRES_POLLING_WRITING:
        watchSocket(false, true);
        break;
    case PGRES_POLLING_OK:
        dropNotifiers();
        if (PQsetnonblocking(m_conn.get(), 1) != 0) {
            fail(QString::fromUtf8(PQerrorMessage(m_conn.get())).trimmed());
            return;
        }
        setState(State::Ready);
        break;
    case PGRES_POLLING_FAILED:
        fail(QString::fromUtf8(PQerrorMessage(m_conn.get())).trimmed());
        break;
    default:
        break;
    }
}

void Connection::watchSocket(bool read, bool write)
{
    // The socket can change between polls (multi-host conninfo, SSL/GSS retry).
    const int socket = PQsocket(m_conn.get());
    if (socket < 0) {
        fail(tr("Connection has no socket"));
        return;
    }
    if (socket != m_socket) {
        dropNotifiers();
        m_socket = socket;
        m_readNotifier = new QSocketNotifier(socket, QSocketNotifier::Read, this);
        m_writeNotifier = new QSocketNotifier(socket, QSocketNotifier::Write, this);
        connect(m_readNotifier, &QSocketNotifier::activated, this, &Connection::pollConnect);
        connect(m_writeNotifier, &QSocketNotifier::activated, this, &Connection::pollConnect);
    }
    m_readNotifier->setEnabled(read);
    m_writeNotifier->setEnabled(write);
}

void Connection::dropNotifiers()
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

void Connection::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

void Connection::fail(const QString &message)
{
    m_error = message;
    dropNotifiers();
    m_conn.reset();
    setState(State::Failed);
}

} // namespace slonisko::pg
