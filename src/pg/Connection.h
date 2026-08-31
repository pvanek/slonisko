// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Handles.h"
#include "pg/Result.h"
#include "pg/SocketWatcher.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <optional>

namespace slonisko::pg {

// One libpq connection driven by the Qt event loop. Never blocks the caller
// and must only be used from the thread that owns it. Signal receivers may
// call any method, including close(), or delete the connection.
class Connection : public QObject
{
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Ready, Busy, Failed };
    Q_ENUM(State)

    // Used when the conninfo does not set connect_timeout.
    static constexpr int DefaultConnectTimeoutMs = 30'000;

    explicit Connection(QObject *parent = nullptr);
    ~Connection() override;

    // Starts a non-blocking connect. conninfo is a libpq connection string or
    // URI. Its connect_timeout (seconds, 0 = none) limits the whole attempt,
    // across all hosts it lists.
    void open(const QByteArray &conninfo);
    void close();

    State state() const { return m_state; }
    // Why the connection failed; empty unless state() == Failed.
    QString errorMessage() const { return m_error; }
    int serverVersion() const;
    int backendPid() const;
    // PQTRANS_IDLE, PQTRANS_INTRANS, PQTRANS_INERROR and so on;
    // PQTRANS_UNKNOWN when not connected.
    PGTransactionStatusType transactionStatus() const;

    // Sends sql, which may hold several statements, over the simple query
    // protocol. Each statement's result arrives through resultReady(), rows in
    // chunks of up to chunkSize if it is positive, then queryFinished(). A
    // failed statement ends the query. If the connection is lost instead,
    // state() becomes Failed and queryFinished() is not emitted. Returns
    // false unless state() == Ready.
    //
    // copyData is sent to the first COPY ... FROM STDIN in sql, in the COPY's
    // format, without the \. end marker (a CopyData span from the splitter).
    // Any other COPY FROM STDIN fails with an error result.
    bool execute(const QByteArray &sql, int chunkSize = 0,
                 std::optional<QByteArray> copyData = std::nullopt);

    // Asks the server, without blocking, to cancel the running query, which
    // then ends with an error result (SQLSTATE 57014). The server may finish
    // the query before the request arrives. Returns false if no query is
    // running, a cancel is already in progress, or the request could not be
    // started; failures are also reported by cancelFailed().
    bool cancel();

Q_SIGNALS:
    void stateChanged(slonisko::pg::Connection::State state);
    void resultReady(const slonisko::pg::Result &result);
    void queryFinished();
    void cancelFailed(const QString &message);
    // A server notice or warning, such as RAISE NOTICE output. Emitted
    // asynchronously, so it may arrive after the result it belongs to.
    void notice(const QString &message);

private:
    void onSocketActivity();
    void pollConnect();
    void readInput();
    void processResults();
    bool sendCopyData();
    bool copyInEnded();
    void connectionLost();
    void watch(bool read, bool write);
    void pollCancel();
    void failCancel();
    void setState(State state);
    void fail(const QString &message);
    QString connectionError() const;
    static void receiveNotice(void *self, const PGresult *notice);

    ConnPtr m_conn;
    State m_state = State::Disconnected;
    QString m_error;
    SocketWatcher m_watcher;
    QTimer m_connectTimer;
    bool m_copyOut = false; // Discarding COPY TO STDOUT data.
    std::optional<QByteArray> m_copyData; // For the query's first COPY FROM STDIN.
    bool m_copyIn = false; // Sending m_copyData.
    qsizetype m_copySent = 0;
    // Why the server is about to close the connection, e.g. an idle timeout.
    // libpq delivers it as a notice, not as the connection's error message.
    QString m_fatalMessage;

    CancelPtr m_cancel;
    SocketWatcher m_cancelWatcher;
};

} // namespace slonisko::pg
