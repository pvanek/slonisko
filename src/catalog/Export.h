// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/RowStore.h"

#include <QString>

#include <vector>

class QTextStream;

namespace slonisko::catalog {

enum class ExportFormat {
    Csv,
    Tsv,
    Text, // The ASCII table the result's text view shows.
    SqlInsert, // One INSERT per row.
    SqlBulkInsert // One INSERT for every few rows.
};

struct ExportOptions
{
    ExportFormat format = ExportFormat::Csv;
    bool header = true; // CSV, TSV and text.
    QChar delimiter = QLatin1Char(','); // CSV; TSV always uses a tab.
    QString nullText; // What a NULL is written as.
    QString table = QStringLiteral("table_name"); // INSERT INTO this.
    int bulkRows = 100; // Rows per bulk INSERT.
    int maxCellWidth = 0; // Text: cut longer cells; 0 keeps them.
    int maxRows = 0; // 0 exports every row.
    std::vector<int> columns; // Empty exports every column.
};

// Writes the rows in the chosen format. Streaming, so a big result needs no
// big string in memory.
void exportRows(const pg::RowStore &rows, const ExportOptions &options, QTextStream &out);
QString exportRows(const pg::RowStore &rows, const ExportOptions &options);

// For save dialogs and menus.
QString formatName(ExportFormat format);
QString fileFilter(ExportFormat format);
QString fileSuffix(ExportFormat format);

// Numbers are right-aligned in the text view and unquoted in SQL.
bool isNumericType(Oid type);

} // namespace slonisko::catalog
