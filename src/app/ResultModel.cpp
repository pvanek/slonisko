// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultModel.h"

#include <QCollator>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QLocale>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>
#include <map>
#include <numeric>

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

// A size as the program's queries write them, such as "8192 B" or
// "1.5 GB", or as pg_size_pretty() itself does ("8192 bytes"), in bytes;
// nullopt for anything else.
std::optional<double> prettySize(const QString &text)
{
    static const QRegularExpression size(
        QStringLiteral(R"(^(-?\d+(?:\.\d+)?) (B|bytes|kB|MB|GB|TB|PB)$)"));
    const QRegularExpressionMatch m = size.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    static const QStringList units {QStringLiteral("B"),  QStringLiteral("kB"),
                                    QStringLiteral("MB"), QStringLiteral("GB"),
                                    QStringLiteral("TB"), QStringLiteral("PB")};
    double bytes = m.captured(1).toDouble();
    for (qsizetype i = units.indexOf(m.captured(2)); i > 0; --i) // "bytes": -1, as B.
        bytes *= 1024;
    return bytes;
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
    m_order.clear();
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
        applySort();
        return;
    }
    if (result.rowCount() == 0)
        return;
    // Fetched rows go before rows the user added; sorted, they go at the end
    // first, then to their places.
    const int first = m_rows.rowCount();
    beginInsertRows({}, first, first + result.rowCount() - 1);
    m_rows.append(result);
    if (!m_order.empty()) {
        for (int row = first; row < m_rows.rowCount(); ++row)
            m_order.push_back(row);
    }
    endInsertRows();
    applySort();
}

void ResultModel::setEditTarget(const catalog::EditTarget &target)
{
    // Edits are kept by a row's place; editable rows stay where they came.
    if (target.editable()) {
        m_sortColumn = -1;
        applySort();
    }
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
        return cell ? *cell : Value(); // Unset: shown as DEFAULT, inserted as the default.
    }
    const auto edit = m_edits.constFind({row, column});
    if (edit != m_edits.cend())
        return *edit;
    const int source = sourceRow(row);
    if (m_rows.isNull(source, column))
        return std::nullopt;
    return m_rows.value(source, column).toByteArray();
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

void ResultModel::sort(int column, Qt::SortOrder order)
{
    m_sortColumn = column;
    m_sortOrder = order;
    applySort();
}

void ResultModel::applySort()
{
    if (isEditable() || hasChanges())
        return;
    const int rows = m_rows.rowCount();
    const int column = m_sortColumn < columnCount() ? m_sortColumn : -1;
    if (column < 0 && m_order.empty())
        return; // Already as the rows came.

    std::vector<int> order;
    if (column >= 0) {
        // The keys once, not once per comparison.
        std::vector<std::optional<QString>> texts(static_cast<std::size_t>(rows));
        std::vector<double> numbers(static_cast<std::size_t>(rows));
        bool allNumbers = true;
        bool allSizes = true;
        for (int row = 0; row < rows; ++row) {
            if (m_rows.isNull(row, column))
                continue;
            const QString text = QString::fromUtf8(m_rows.value(row, column));
            texts[std::size_t(row)] = text;
            bool ok = false;
            numbers[std::size_t(row)] = text.toDouble(&ok);
            allNumbers = allNumbers && ok;
            if (allSizes && !ok) {
                const std::optional<double> bytes = prettySize(text);
                allSizes = bytes.has_value();
                if (bytes)
                    numbers[std::size_t(row)] = *bytes;
            }
        }
        // A column of sizes may hold plain byte counts too; numbers they are.
        const bool byNumber = (m_numeric[std::size_t(column)] && allNumbers) || allSizes;
        // The C locale, which a build host or a bare environment runs in,
        // compares code points and ignores numeric mode: "b10" before "B2".
        QCollator collator(QLocale().language() == QLocale::C
                               ? QLocale(QLocale::English, QLocale::UnitedStates)
                               : QLocale());
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        const bool descending = m_sortOrder == Qt::DescendingOrder;

        order.resize(std::size_t(rows));
        std::iota(order.begin(), order.end(), 0);
        auto compare = [&](int a, int b) {
            if (!byNumber)
                return collator.compare(*texts[std::size_t(a)], *texts[std::size_t(b)]);
            const double x = numbers[std::size_t(a)];
            const double y = numbers[std::size_t(b)];
            return x < y ? -1 : x > y ? 1 : 0;
        };
        std::ranges::stable_sort(order, [&](int a, int b) {
            const bool nullA = !texts[std::size_t(a)];
            const bool nullB = !texts[std::size_t(b)];
            if (nullA || nullB)
                return !nullA && nullB; // NULLs last.
            const int c = compare(a, b);
            return descending ? c > 0 : c < 0;
        });
    }

    Q_EMIT layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    // Selection and the current cell stay on their rows.
    std::vector<int> placeOf(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row)
        placeOf[std::size_t(order.empty() ? row : order[std::size_t(row)])] = row;
    const QModelIndexList before = persistentIndexList();
    QModelIndexList after;
    for (const QModelIndex &i : before) {
        const int source = sourceRow(i.row());
        after << (source < rows ? index(placeOf[std::size_t(source)], i.column()) : i);
    }
    m_order = std::move(order);
    changePersistentIndexList(before, after);
    Q_EMIT layoutChanged({}, QAbstractItemModel::VerticalSortHint);
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
