// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

#include <libpq-fe.h>

#include <memory>

namespace slonisko::pg {

// A libpq result with shared ownership, cheap to copy and pass through signals.
class Result
{
public:
    Result() = default;
    explicit Result(PGresult *result) : m_result(result, PQclear) { }

    bool isNull() const { return !m_result; }
    PGresult *get() const { return m_result.get(); }

    ExecStatusType status() const;
    bool isError() const;
    // One chunk of rows in chunked rows mode; more chunks or the final,
    // empty PGRES_TUPLES_OK result follow.
    bool isChunk() const { return status() == PGRES_TUPLES_CHUNK; }

    int rowCount() const;
    int columnCount() const;
    QString columnName(int column) const;
    Oid columnType(int column) const;
    bool isNull(int row, int column) const;
    QByteArray value(int row, int column) const;

    // Such as "INSERT 0 2" or "SELECT 1".
    QByteArray commandTag() const;

    QString errorMessage() const;
    QByteArray sqlState() const;
    // 1-based character position of the error in the query, 0 if none.
    int errorPosition() const;

private:
    std::shared_ptr<PGresult> m_result;
};

} // namespace slonisko::pg

Q_DECLARE_METATYPE(slonisko::pg::Result)
