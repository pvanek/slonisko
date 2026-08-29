// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/Result.h"

#include <QAbstractTableModel>

namespace slonisko {

// The rows of one query result.
class ResultModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    using QAbstractTableModel::QAbstractTableModel;

    void setResult(const pg::Result &result);
    void clear() { setResult({}); }
    const pg::Result &result() const { return m_result; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    pg::Result m_result;
    std::vector<bool> m_numeric; // Per column: right-align.
};

} // namespace slonisko
