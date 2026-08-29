// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"

#include <QFont>
#include <QGuiApplication>
#include <QPalette>

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

void ResultModel::setResult(const pg::Result &result)
{
    beginResetModel();
    m_result = result;
    m_numeric.assign(std::size_t(m_result.columnCount()), false);
    for (int c = 0; c < m_result.columnCount(); ++c)
        m_numeric[std::size_t(c)] = isNumeric(m_result.columnType(c));
    endResetModel();
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() || m_result.isNull() ? 0 : m_result.rowCount();
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() || m_result.isNull() ? 0 : m_result.columnCount();
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const int row = index.row();
    const int column = index.column();
    const bool null = m_result.isNull(row, column);

    switch (role) {
    case Qt::DisplayRole: {
        if (null)
            return QStringLiteral("NULL");
        QString text = QString::fromUtf8(m_result.value(row, column));
        if (text.size() > MaxCellLength)
            text = text.left(MaxCellLength) + QChar(0x2026);
        return text.replace(QLatin1Char('\n'), QChar(0x21B5));
    }
    case Qt::ToolTipRole:
        return null
            ? QVariant()
            : QVariant(QString::fromUtf8(m_result.value(row, column).left(4 * MaxCellLength)));
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
    return m_result.columnName(section);
}

} // namespace slonisko
