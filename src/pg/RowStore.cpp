// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pg/RowStore.h"

#include <algorithm>

namespace slonisko::pg {

void RowStore::clear()
{
    m_columns.clear();
    m_chunks.clear();
    m_firstRow.clear();
    m_rows = 0;
}

void RowStore::append(const Result &result)
{
    if (result.isNull() || result.columnCount() == 0)
        return;
    if (m_columns.empty()) {
        for (int c = 0; c < result.columnCount(); ++c)
            m_columns.push_back({result.columnName(c), result.columnType(c),
                                 result.columnTypeModifier(c), result.columnTable(c),
                                 result.columnTableColumn(c)});
    }
    const int rows = result.rowCount();
    const int columns = int(m_columns.size());
    if (rows == 0 || result.columnCount() != columns)
        return;

    PGresult *r = result.get();
    Chunk chunk;
    chunk.rows = rows;
    qsizetype size = 0;
    for (int row = 0; row < rows; ++row)
        for (int c = 0; c < columns; ++c)
            size += PQgetlength(r, row, c);
    chunk.data.reserve(size);
    chunk.offsets.reserve(std::size_t(rows) * std::size_t(columns) + 1);
    chunk.nulls.reserve(std::size_t(rows) * std::size_t(columns));
    for (int row = 0; row < rows; ++row) {
        for (int c = 0; c < columns; ++c) {
            chunk.offsets.push_back(std::uint32_t(chunk.data.size()));
            chunk.nulls.push_back(PQgetisnull(r, row, c) != 0);
            chunk.data.append(PQgetvalue(r, row, c), PQgetlength(r, row, c));
        }
    }
    chunk.offsets.push_back(std::uint32_t(chunk.data.size()));
    m_firstRow.push_back(m_rows);
    m_rows += rows;
    m_chunks.push_back(std::move(chunk));
}

std::pair<const RowStore::Chunk *, std::size_t> RowStore::cell(int row, int column) const
{
    const auto it = std::upper_bound(m_firstRow.begin(), m_firstRow.end(), row);
    const auto index = std::size_t(it - m_firstRow.begin()) - 1;
    const Chunk &chunk = m_chunks[index];
    return {&chunk, std::size_t(row - m_firstRow[index]) * m_columns.size() + std::size_t(column)};
}

bool RowStore::isNull(int row, int column) const
{
    const auto [chunk, i] = cell(row, column);
    return chunk->nulls[i];
}

QByteArrayView RowStore::value(int row, int column) const
{
    const auto [chunk, i] = cell(row, column);
    return QByteArrayView(chunk->data)
        .sliced(chunk->offsets[i], chunk->offsets[i + 1] - chunk->offsets[i]);
}

qsizetype RowStore::memoryUsed() const
{
    qsizetype bytes = 0;
    for (const Chunk &c : m_chunks)
        bytes += c.data.capacity() + qsizetype(c.offsets.capacity() * sizeof(std::uint32_t))
            + qsizetype(c.nulls.capacity() / 8);
    return bytes;
}

} // namespace slonisko::pg
