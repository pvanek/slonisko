// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>

#include <algorithm>
#include <map>

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

bool isDark()
{
    return QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
}

} // namespace

void ResultModel::clear()
{
    beginResetModel();
    m_rows.clear();
    m_numeric.clear();
    m_target = {};
    m_edits.clear();
    m_deleted.clear();
    m_added.clear();
    endResetModel();
    Q_EMIT changesChanged();
    Q_EMIT editTargetChanged();
}

void ResultModel::append(const pg::Result &result)
{
    if (result.isNull() || result.columnCount() == 0)
        return;
    if (!m_rows.hasColumns()) {
        beginResetModel();
        m_rows.append(result);
        m_numeric.assign(std::size_t(m_rows.columnCount()), false);
        for (int c = 0; c < m_rows.columnCount(); ++c)
            m_numeric[std::size_t(c)] = isNumeric(m_rows.column(c).type);
        endResetModel();
        return;
    }
    if (result.rowCount() == 0)
        return;
    // Fetched rows go before rows the user added.
    const int first = m_rows.rowCount();
    beginInsertRows({}, first, first + result.rowCount() - 1);
    m_rows.append(result);
    endInsertRows();
}

void ResultModel::setEditTarget(const catalog::EditTarget &target)
{
    m_target = target;
    if (rowCount() > 0 && columnCount() > 0)
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    Q_EMIT editTargetChanged();
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.rowCount() + int(m_added.size());
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.columnCount();
}

ResultModel::Value ResultModel::value(int row, int column) const
{
    if (isAdded(row)) {
        const auto &cell = m_added[std::size_t(row - m_rows.rowCount())][std::size_t(column)];
        return cell ? *cell : Value(); // Unset: shown as NULL, inserted as the default.
    }
    const auto edit = m_edits.constFind({row, column});
    if (edit != m_edits.cend())
        return *edit;
    if (m_rows.isNull(row, column))
        return std::nullopt;
    return m_rows.value(row, column).toByteArray();
}

bool ResultModel::isEdited(int row, int column) const
{
    if (isAdded(row))
        return m_added[std::size_t(row - m_rows.rowCount())][std::size_t(column)].has_value();
    return m_edits.contains({row, column});
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const int row = index.row();
    const int column = index.column();
    const Value v = value(row, column);
    const bool added = isAdded(row);
    const bool unset = added && !isEdited(row, column);

    switch (role) {
    case Qt::DisplayRole: {
        if (unset)
            return QStringLiteral("DEFAULT");
        if (!v)
            return QStringLiteral("NULL");
        QString text = QString::fromUtf8(*v);
        if (text.size() > MaxCellLength)
            text = text.left(MaxCellLength) + QChar(0x2026);
        return text.replace(QLatin1Char('\n'), QChar(0x21B5));
    }
    case Qt::EditRole:
        return v ? QVariant(QString::fromUtf8(*v)) : QVariant(QString());
    case Qt::ToolTipRole:
        return v ? QVariant(QString::fromUtf8(v->left(4 * MaxCellLength))) : QVariant();
    case Qt::TextAlignmentRole:
        return m_numeric[std::size_t(column)] ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                              : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ForegroundRole:
        return !v || unset ? QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text)
                           : QVariant();
    case Qt::BackgroundRole: {
        const bool dark = isDark();
        if (m_deleted.contains(row))
            return dark ? QColor(110, 40, 40) : QColor(255, 215, 215);
        if (added)
            return dark ? QColor(40, 90, 50) : QColor(215, 245, 215);
        if (isEdited(row, column))
            return dark ? QColor(110, 95, 30) : QColor(255, 243, 190);
        return {};
    }
    case Qt::FontRole: {
        QFont font;
        font.setItalic(!v || unset);
        font.setStrikeOut(m_deleted.contains(row));
        return font;
    }
    default:
        return {};
    }
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};
    if (orientation == Qt::Vertical)
        return isAdded(section) ? QVariant(QStringLiteral("*")) : QVariant(section + 1);
    return m_rows.hasColumns() ? QVariant(m_rows.column(section).name) : QVariant();
}

Qt::ItemFlags ResultModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && m_target.writable(index.column()) && !m_deleted.contains(index.row()))
        f |= Qt::ItemIsEditable;
    return f;
}

bool ResultModel::setData(const QModelIndex &index, const QVariant &v, int role)
{
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable))
        return false;
    setValue(index.row(), index.column(), v.isNull() ? Value() : Value(v.toString().toUtf8()));
    return true;
}

void ResultModel::setValue(int row, int column, const Value &v)
{
    if (isAdded(row)) {
        m_added[std::size_t(row - m_rows.rowCount())][std::size_t(column)] = v;
    } else {
        const Value original
            = m_rows.isNull(row, column) ? Value() : Value(m_rows.value(row, column).toByteArray());
        if (v == original)
            m_edits.remove({row, column}); // Back to what it was.
        else
            m_edits.insert({row, column}, v);
    }
    const QModelIndex i = index(row, column);
    Q_EMIT dataChanged(i, i);
    Q_EMIT changesChanged();
}

void ResultModel::setNull(const QModelIndexList &cells)
{
    for (const QModelIndex &i : cells) {
        if (flags(i) & Qt::ItemIsEditable)
            setValue(i.row(), i.column(), std::nullopt);
    }
}

int ResultModel::addRow()
{
    const int row = rowCount();
    beginInsertRows({}, row, row);
    m_added.emplace_back(std::size_t(columnCount()));
    endInsertRows();
    Q_EMIT changesChanged();
    return row;
}

void ResultModel::toggleDeleted(const QList<int> &rows)
{
    // Added rows go away, from the last, so indexes stay valid.
    QList<int> sorted = rows;
    std::ranges::sort(sorted, std::greater<>());
    for (const int row : sorted) {
        if (isAdded(row)) {
            beginRemoveRows({}, row, row);
            m_added.erase(m_added.begin() + (row - m_rows.rowCount()));
            endRemoveRows();
        } else if (!m_deleted.remove(row)) {
            m_deleted.insert(row);
        }
        if (!isAdded(row) && row < rowCount())
            Q_EMIT dataChanged(index(row, 0), index(row, columnCount() - 1));
    }
    Q_EMIT changesChanged();
}

bool ResultModel::hasChanges() const
{
    return !m_edits.isEmpty() || !m_deleted.isEmpty() || !m_added.empty();
}

std::vector<catalog::RowEdit> ResultModel::changes() const
{
    using Edit = catalog::RowEdit;
    auto keyOf = [&](int row) {
        std::vector<Value> key;
        for (const int column : m_target.key)
            key.push_back(m_rows.isNull(row, column)
                              ? Value()
                              : Value(m_rows.value(row, column).toByteArray()));
        return key;
    };

    std::vector<Edit> out;
    QList<int> deleted(m_deleted.begin(), m_deleted.end());
    std::ranges::sort(deleted);
    for (const int row : deleted)
        out.push_back({Edit::Kind::Delete, keyOf(row), {}});

    // Updates, one per row, columns in order.
    std::map<int, std::vector<std::pair<int, Value>>> updates;
    for (auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
        if (!m_deleted.contains(it.key().first))
            updates[it.key().first].push_back({it.key().second, *it});
    }
    for (auto &[row, values] : updates) {
        std::ranges::sort(values, {}, &std::pair<int, Value>::first);
        out.push_back({Edit::Kind::Update, keyOf(row), values});
    }

    for (const auto &added : m_added) {
        Edit e {Edit::Kind::Insert, {}, {}};
        for (int column = 0; column < int(added.size()); ++column) {
            if (added[std::size_t(column)])
                e.values.push_back({column, *added[std::size_t(column)]});
        }
        out.push_back(std::move(e));
    }
    return out;
}

void ResultModel::discardChanges()
{
    beginResetModel();
    m_edits.clear();
    m_deleted.clear();
    m_added.clear();
    endResetModel();
    Q_EMIT changesChanged();
}

} // namespace slonisko
