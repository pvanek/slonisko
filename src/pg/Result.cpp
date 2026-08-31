// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/Result.h"

namespace slonisko::pg {

ExecStatusType Result::status() const
{
    return m_result ? PQresultStatus(m_result.get()) : PGRES_FATAL_ERROR;
}

bool Result::isError() const
{
    const ExecStatusType s = status();
    return s == PGRES_FATAL_ERROR || s == PGRES_BAD_RESPONSE || s == PGRES_NONFATAL_ERROR;
}

int Result::rowCount() const
{
    return m_result ? PQntuples(m_result.get()) : 0;
}

int Result::columnCount() const
{
    return m_result ? PQnfields(m_result.get()) : 0;
}

QString Result::columnName(int column) const
{
    return QString::fromUtf8(PQfname(m_result.get(), column));
}

Oid Result::columnType(int column) const
{
    return PQftype(m_result.get(), column);
}

int Result::columnTypeModifier(int column) const
{
    return PQfmod(m_result.get(), column);
}

Oid Result::columnTable(int column) const
{
    return PQftable(m_result.get(), column);
}

int Result::columnTableColumn(int column) const
{
    return PQftablecol(m_result.get(), column);
}

bool Result::isNull(int row, int column) const
{
    return PQgetisnull(m_result.get(), row, column) != 0;
}

QByteArray Result::value(int row, int column) const
{
    return QByteArray(PQgetvalue(m_result.get(), row, column),
                      PQgetlength(m_result.get(), row, column));
}

QByteArray Result::commandTag() const
{
    return m_result ? QByteArray(PQcmdStatus(m_result.get())) : QByteArray();
}

QString Result::errorMessage() const
{
    return m_result ? QString::fromUtf8(PQresultErrorMessage(m_result.get())).trimmed() : QString();
}

QByteArray Result::sqlState() const
{
    return m_result ? QByteArray(PQresultErrorField(m_result.get(), PG_DIAG_SQLSTATE))
                    : QByteArray();
}

int Result::errorPosition() const
{
    if (!m_result)
        return 0;
    const char *position = PQresultErrorField(m_result.get(), PG_DIAG_STATEMENT_POSITION);
    return position ? QByteArray(position).toInt() : 0;
}

} // namespace slonisko::pg
