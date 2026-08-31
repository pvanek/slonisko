// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <vector>

namespace slonisko::pg {

// The rows of a query result, copied out of libpq's results as they arrive
// so those can be freed: per chunk one byte buffer with every cell's text,
// and an offset table. Values stay in PostgreSQL's text form, as sent.
class RowStore
{
public:
    struct Column
    {
        QString name;
        Oid type = 0;
        int typeModifier = -1;
        Oid table = 0; // PQftable: 0 if computed.
        int tableColumn = 0; // PQftablecol: attribute number in that table.
    };

    void clear();
    // Copies a result's rows. The first result with columns sets them; later
    // ones are chunks of the same query.
    void append(const Result &result);

    int rowCount() const { return m_rows; }
    int columnCount() const { return int(m_columns.size()); }
    bool hasColumns() const { return !m_columns.empty(); }
    const Column &column(int c) const { return m_columns[std::size_t(c)]; }

    bool isNull(int row, int column) const;
    QByteArrayView value(int row, int column) const;

    // Bytes held, for the status line.
    qsizetype memoryUsed() const;

private:
    struct Chunk
    {
        QByteArray data;
        std::vector<std::uint32_t> offsets; // rows * columns + 1: where each cell starts.
        std::vector<bool> nulls;
        int rows = 0;
    };
    std::pair<const Chunk *, std::size_t> cell(int row, int column) const;

    std::vector<Column> m_columns;
    std::vector<Chunk> m_chunks;
    std::vector<int> m_firstRow; // Of each chunk.
    int m_rows = 0;
};

} // namespace slonisko::pg
