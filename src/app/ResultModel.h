// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Result.h"

#include <QAbstractTableModel>

#include <vector>

namespace slonisko {

// The rows of one query, which may arrive in chunks.
class ResultModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    using QAbstractTableModel::QAbstractTableModel;

    void clear();
    // Adds a result's rows. The first result sets the columns; later ones
    // are chunks of the same query and must have the same columns.
    void append(const pg::Result &result);
    void setResult(const pg::Result &result)
    {
        clear();
        append(result);
    }
    bool hasColumns() const { return !m_chunks.empty(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    // Which chunk a row is in, and its row within that chunk.
    std::pair<const pg::Result *, int> locate(int row) const;

    std::vector<pg::Result> m_chunks;
    std::vector<int> m_firstRow; // Of each chunk.
    int m_rows = 0;
    std::vector<bool> m_numeric; // Per column: right-align.
};

} // namespace slonisko
