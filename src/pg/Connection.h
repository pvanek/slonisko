// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Handles.h"

#include <QObject>
#include <QString>

class QSocketNotifier;

namespace slonisko::pg {

// One libpq connection driven by the Qt event loop. Never blocks the caller
// and must only be used from the thread that owns it.
class Connection : public QObject
{
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Ready, Failed };
    Q_ENUM(State)

    explicit Connection(QObject *parent = nullptr);
    ~Connection() override;

    // Starts a non-blocking connect. conninfo is a libpq connection string or URI.
    void open(const QByteArray &conninfo);
    void close();

    State state() const { return m_state; }
    QString errorMessage() const { return m_error; }
    int serverVersion() const;

    // Raw handle for the query layer; null unless state() == Ready.
    PGconn *handle() const { return m_state == State::Ready ? m_conn.get() : nullptr; }

Q_SIGNALS:
    void stateChanged(slonisko::pg::Connection::State state);

private:
    void pollConnect();
    void watchSocket(bool read, bool write);
    void dropNotifiers();
    void setState(State state);
    void fail(const QString &message);

    ConnPtr m_conn;
    State m_state = State::Disconnected;
    QString m_error;
    int m_socket = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
};

} // namespace slonisko::pg
