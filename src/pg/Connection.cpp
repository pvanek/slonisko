// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/Connection.h"

#include <QPointer>

#include <algorithm>
#include <cstring>

namespace slonisko::pg {

namespace {

// connect_timeout as libpq resolved it, including PGCONNECT_TIMEOUT.
int connectTimeoutMs(PGconn *conn)
{
    const ConninfoPtr options(PQconninfo(conn));
    for (const PQconninfoOption *o = options.get(); o && o->keyword; ++o) {
        if (std::strcmp(o->keyword, "connect_timeout") == 0 && o->val && *o->val)
            return QByteArray(o->val).toInt() * 1000;
    }
    return Connection::DefaultConnectTimeoutMs;
}

// Large enough to keep the socket busy, small enough not to double memory use.
constexpr qsizetype CopyChunkSize = 64 * 1024;

} // namespace

Connection::Connection(QObject *parent) : QObject(parent)
{
    m_connectTimer.setSingleShot(true);
    connect(&m_connectTimer, &QTimer::timeout, this,
            [this] { fail(tr("Timed out connecting to the server")); });
    connect(&m_watcher, &SocketWatcher::activated, this, &Connection::onSocketActivity);
    connect(&m_cancelWatcher, &SocketWatcher::activated, this, &Connection::pollCancel);
}

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
    PQsetNoticeReceiver(m_conn.get(), &Connection::receiveNotice, this);
    if (PQstatus(m_conn.get()) == CONNECTION_BAD) {
        fail(connectionError());
        return;
    }

    // Non-blocking connects ignore connect_timeout, so enforce it here.
    const int timeout = connectTimeoutMs(m_conn.get());
    if (timeout > 0)
        m_connectTimer.start(timeout);

    setState(State::Connecting);
    // libpq: after PQconnectStart, behave as if PQconnectPoll returned PGRES_POLLING_WRITING.
    watch(false, true);
}

void Connection::close()
{
    m_connectTimer.stop();
    m_watcher.stop();
    m_cancelWatcher.stop();
    m_cancel.reset();
    m_conn.reset();
    m_copyOut = false;
    m_copyIn = false;
    m_copyData.reset();
    m_fatalMessage.clear();
    m_error.clear();
    setState(State::Disconnected);
}

int Connection::serverVersion() const
{
    return m_conn ? PQserverVersion(m_conn.get()) : 0;
}

PGTransactionStatusType Connection::transactionStatus() const
{
    return m_conn ? PQtransactionStatus(m_conn.get()) : PQTRANS_UNKNOWN;
}

int Connection::backendPid() const
{
    return m_conn ? PQbackendPID(m_conn.get()) : 0;
}

bool Connection::execute(const QByteArray &sql, int chunkSize, std::optional<QByteArray> copyData)
{
    if (m_state != State::Ready)
        return false;

    PGconn *conn = m_conn.get();
    if (!PQsendQuery(conn, sql.constData())) {
        // The query was not sent; the connection is most likely gone.
        connectionLost();
        return false;
    }
    if (chunkSize > 0)
        PQsetChunkedRowsMode(conn, chunkSize);
    m_copyData = std::move(copyData);

    setState(State::Busy);
    // In non-blocking mode the query may not be fully sent yet.
    watch(true, PQflush(conn) == 1);
    return true;
}

bool Connection::cancel()
{
    if (m_state != State::Busy || m_cancel)
        return false;

    m_cancel.reset(PQcancelCreate(m_conn.get()));
    if (!m_cancel || !PQcancelStart(m_cancel.get())) {
        failCancel();
        return false;
    }
    // libpq: after PQcancelStart, behave as if PQcancelPoll returned PGRES_POLLING_WRITING.
    m_cancelWatcher.watch(PQcancelSocket(m_cancel.get()), false, true);
    return true;
}

void Connection::onSocketActivity()
{
    if (m_state == State::Connecting)
        pollConnect();
    else if (m_state == State::Ready || m_state == State::Busy)
        readInput();
}

void Connection::pollConnect()
{
    switch (PQconnectPoll(m_conn.get())) {
    case PGRES_POLLING_READING:
        watch(true, false);
        break;
    case PGRES_POLLING_WRITING:
        watch(false, true);
        break;
    case PGRES_POLLING_OK:
        m_connectTimer.stop();
        if (PQsetnonblocking(m_conn.get(), 1) != 0) {
            fail(connectionError());
            return;
        }
        // Keep reading while idle to notice when the server closes the connection.
        watch(true, false);
        if (m_state == State::Connecting)
            setState(State::Ready);
        break;
    case PGRES_POLLING_FAILED:
        fail(connectionError());
        break;
    default:
        break;
    }
}

void Connection::readInput()
{
    PGconn *conn = m_conn.get();
    // Keep reading while the query is still being sent, or a server that is
    // itself blocked on sending could deadlock us.
    const int unsent = m_state == State::Busy ? PQflush(conn) : 0;
    if (unsent < 0 || !PQconsumeInput(conn)) {
        connectionLost();
        return;
    }
    // Notifications are not exposed yet; free them so they do not pile up.
    while (NotifyPtr(PQnotifies(conn))) { }

    if (m_state == State::Busy) {
        watch(true, unsent == 1);
        processResults();
    } else if (PQstatus(conn) == CONNECTION_BAD) {
        connectionLost();
    }
}

void Connection::processResults()
{
    const QPointer<Connection> guard(this);
    PGconn *conn = m_conn.get();

    while (m_state == State::Busy) {
        if (m_copyOut) {
            char *buffer = nullptr;
            int length = 0;
            while ((length = PQgetCopyData(conn, &buffer, 1)) > 0)
                PQfreemem(buffer);
            if (length == 0)
                return; // Wait for more data.
            m_copyOut = false;
        }
        // While copying in, libpq keeps returning the COPY_IN result.
        if (m_copyIn && !sendCopyData())
            return;
        if (PQisBusy(conn))
            return;

        PGresult *raw = PQgetResult(conn);
        if (!raw) {
            setState(State::Ready);
            if (guard)
                Q_EMIT queryFinished();
            return;
        }

        const Result result(raw);
        if (result.status() == PGRES_COPY_IN) {
            if (m_copyData) {
                m_copyIn = true;
                m_copySent = 0;
            } else if (PQputCopyEnd(conn, "No data for COPY FROM STDIN") < 0 || PQflush(conn) < 0) {
                // Fail the COPY instead of leaving it waiting forever.
                connectionLost();
                return;
            }
        } else if (result.status() == PGRES_COPY_OUT) {
            m_copyOut = true;
        }

        Q_EMIT resultReady(result);
        if (!guard)
            return;
    }
}

// Sends as much of m_copyData as the socket takes, then ends the COPY.
// Returns true once the COPY has ended, false to wait for the socket.
bool Connection::sendCopyData()
{
    PGconn *conn = m_conn.get();
    const QByteArray &data = *m_copyData;
    while (m_copySent < data.size()) {
        // libpq would buffer everything in non-blocking mode, so only queue
        // more once the previous chunk has reached the socket, bounding memory
        // use. (A server error cannot stop the sending early: libpq only reads
        // it after PQputCopyEnd, and the server discards the rest meanwhile.)
        const int unsent = PQflush(conn);
        if (unsent < 0) {
            connectionLost();
            return false;
        }
        if (unsent == 1) {
            watch(true, true);
            return false;
        }

        const auto length = static_cast<int>(std::min(data.size() - m_copySent, CopyChunkSize));
        const int sent = PQputCopyData(conn, data.constData() + m_copySent, length);
        if (sent == 0) {
            watch(true, true);
            return false;
        }
        if (sent < 0)
            return copyInEnded();
        m_copySent += length;
    }

    const int ended = PQputCopyEnd(conn, nullptr);
    if (ended == 0) {
        watch(true, true);
        return false;
    }
    if (ended < 0)
        return copyInEnded();

    const int unsent = PQflush(conn);
    if (unsent < 0) {
        connectionLost();
        return false;
    }
    m_copyIn = false;
    m_copyData.reset();
    watch(true, unsent == 1);
    return m_state == State::Busy;
}

// libpq refused more COPY data. That normally means the connection broke;
// otherwise the COPY ended some other way and its result is waiting.
bool Connection::copyInEnded()
{
    m_copyIn = false;
    m_copyData.reset();
    if (PQstatus(m_conn.get()) == CONNECTION_BAD) {
        connectionLost();
        return false;
    }
    return true;
}

void Connection::connectionLost()
{
    // Deliver what arrived before the connection broke, usually the error
    // that explains it, such as a terminated backend.
    const QPointer<Connection> guard(this);
    if (m_state == State::Busy) {
        for (int i = 0; i < 16; ++i) {
            PGresult *raw = PQgetResult(m_conn.get());
            if (!raw)
                break;
            Q_EMIT resultReady(Result(raw));
            if (!guard || m_state != State::Busy)
                return;
        }
    }
    fail(m_fatalMessage.isEmpty() ? connectionError() : m_fatalMessage);
}

void Connection::watch(bool read, bool write)
{
    const int socket = PQsocket(m_conn.get());
    if (socket < 0) {
        fail(tr("Connection has no socket"));
        return;
    }
    m_watcher.watch(socket, read, write);
}

void Connection::pollCancel()
{
    switch (PQcancelPoll(m_cancel.get())) {
    case PGRES_POLLING_READING:
        m_cancelWatcher.watch(PQcancelSocket(m_cancel.get()), true, false);
        break;
    case PGRES_POLLING_WRITING:
        m_cancelWatcher.watch(PQcancelSocket(m_cancel.get()), false, true);
        break;
    case PGRES_POLLING_OK:
        m_cancelWatcher.stop();
        m_cancel.reset();
        break;
    case PGRES_POLLING_FAILED:
        failCancel();
        break;
    default:
        break;
    }
}

void Connection::failCancel()
{
    const QString message = m_cancel
        ? QString::fromUtf8(PQcancelErrorMessage(m_cancel.get())).trimmed()
        : tr("Could not allocate cancel request");
    m_cancelWatcher.stop();
    m_cancel.reset();
    Q_EMIT cancelFailed(message);
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
    m_connectTimer.stop();
    m_watcher.stop();
    m_cancelWatcher.stop();
    m_cancel.reset();
    m_conn.reset();
    m_copyOut = false;
    m_copyIn = false;
    m_copyData.reset();
    m_fatalMessage.clear();
    m_error = message;
    setState(State::Failed);
}

void Connection::receiveNotice(void *self, const PGresult *notice)
{
    auto *connection = static_cast<Connection *>(self);
    const QString message = QString::fromUtf8(PQresultErrorMessage(notice)).trimmed();
    const QByteArray severity = PQresultErrorField(notice, PG_DIAG_SEVERITY_NONLOCALIZED);
    if (severity == "FATAL" || severity == "PANIC")
        connection->m_fatalMessage = message;
    // Called from inside libpq; let receivers run once it has returned.
    QMetaObject::invokeMethod(
        connection, [connection, message] { Q_EMIT connection->notice(message); },
        Qt::QueuedConnection);
}

QString Connection::connectionError() const
{
    return QString::fromUtf8(PQerrorMessage(m_conn.get())).trimmed();
}

} // namespace slonisko::pg
