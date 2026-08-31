// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"

#include <QFont>
#include <QGuiApplication>
#include <QPalette>

#include <algorithm>

namespace slonisko {

namespace {

// Longer values are cut short in cells; the tooltip has more.
constexpr int MaxCellLength = 500;

bool isNumeric(Oid type)
{
    switch (type) {
    case 20: // int8
    case 21: // int2
    case 23: // int4
    case 26: // oid
    case 700: // float4
    case 701: // float8
    case 790: // money
    case 1700: // numeric
        return true;
    default:
        return false;
    }
}

} // namespace

void ResultModel::clear()
{
    beginResetModel();
    m_chunks.clear();
    m_firstRow.clear();
    m_rows = 0;
    m_numeric.clear();
    endResetModel();
}

void ResultModel::append(const pg::Result &result)
{
    if (result.isNull() || result.columnCount() == 0)
        return;
    if (m_chunks.empty()) {
        beginResetModel();
        m_chunks.push_back(result);
        m_firstRow.push_back(0);
        m_rows = result.rowCount();
        m_numeric.assign(std::size_t(result.columnCount()), false);
        for (int c = 0; c < result.columnCount(); ++c)
            m_numeric[std::size_t(c)] = isNumeric(result.columnType(c));
        endResetModel();
        return;
    }
    if (result.rowCount() == 0)
        return;
    beginInsertRows({}, m_rows, m_rows + result.rowCount() - 1);
    m_chunks.push_back(result);
    m_firstRow.push_back(m_rows);
    m_rows += result.rowCount();
    endInsertRows();
}

std::pair<const pg::Result *, int> ResultModel::locate(int row) const
{
    const auto it = std::upper_bound(m_firstRow.begin(), m_firstRow.end(), row);
    const auto chunk = std::size_t(it - m_firstRow.begin()) - 1;
    return {&m_chunks[chunk], row - m_firstRow[chunk]};
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows;
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() || m_chunks.empty() ? 0 : m_chunks.front().columnCount();
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const auto [chunk, row] = locate(index.row());
    const pg::Result &result = *chunk;
    const int column = index.column();
    const bool null = result.isNull(row, column);

    switch (role) {
    case Qt::DisplayRole: {
        if (null)
            return QStringLiteral("NULL");
        QString text = QString::fromUtf8(result.value(row, column));
        if (text.size() > MaxCellLength)
            text = text.left(MaxCellLength) + QChar(0x2026);
        return text.replace(QLatin1Char('\n'), QChar(0x21B5));
    }
    case Qt::ToolTipRole:
        return null
            ? QVariant()
            : QVariant(QString::fromUtf8(result.value(row, column).left(4 * MaxCellLength)));
    case Qt::TextAlignmentRole:
        return m_numeric[std::size_t(column)] ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                              : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ForegroundRole:
        return null ? QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text)
                    : QVariant();
    case Qt::FontRole:
        if (null) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        return {};
    default:
        return {};
    }
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};
    if (orientation == Qt::Vertical)
        return section + 1;
    return m_chunks.empty() ? QVariant() : m_chunks.front().columnName(section);
}

} // namespace slonisko
