// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Export.h"

#include "catalog/Scope.h"

#include <QCoreApplication>
#include <QTextStream>

#include <algorithm>

namespace slonisko::catalog {

namespace {

std::vector<int> columnsOf(const pg::RowStore &rows, const ExportOptions &options)
{
    if (!options.columns.empty())
        return options.columns;
    std::vector<int> all(std::size_t(rows.columnCount()));
    for (int c = 0; c < rows.columnCount(); ++c)
        all[std::size_t(c)] = c;
    return all;
}

int rowsOf(const pg::RowStore &rows, const ExportOptions &options)
{
    return options.maxRows > 0 ? std::min(options.maxRows, rows.rowCount()) : rows.rowCount();
}

QString text(const pg::RowStore &rows, int row, int column)
{
    return QString::fromUtf8(rows.value(row, column).toByteArray());
}

// RFC 4180: quote a field holding the delimiter, a quote, a line break or
// spaces at its ends, and double the quotes inside it.
QString csvField(const QString &value, QChar delimiter)
{
    const bool quote = value.contains(delimiter) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))
        || (!value.isEmpty() && (value.front().isSpace() || value.back().isSpace()));
    if (!quote)
        return value;
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

void writeSeparated(const pg::RowStore &rows, const ExportOptions &options, QChar delimiter,
                    QTextStream &out)
{
    const std::vector<int> columns = columnsOf(rows, options);
    if (options.header) {
        for (std::size_t i = 0; i < columns.size(); ++i)
            out << (i ? QString(delimiter) : QString())
                << csvField(rows.column(columns[i]).name, delimiter);
        out << '\n';
    }
    const int last = rowsOf(rows, options);
    for (int row = 0; row < last; ++row) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const int column = columns[i];
            if (i)
                out << delimiter;
            out << (rows.isNull(row, column) ? options.nullText
                                             : csvField(text(rows, row, column), delimiter));
        }
        out << '\n';
    }
}

QString cut(const QString &value, int maxWidth)
{
    if (maxWidth <= 0 || value.size() <= maxWidth)
        return value;
    return value.left(std::max(1, maxWidth - 1)) + QChar(0x2026); // …
}

// A cell on one line: control characters would break the table apart.
QString oneLine(const QString &value)
{
    QString out = value;
    out.replace(QLatin1Char('\t'), QLatin1String("    "));
    out.replace(QLatin1Char('\r'), QString());
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return out;
}

void writeText(const pg::RowStore &rows, const ExportOptions &options, QTextStream &out)
{
    const std::vector<int> columns = columnsOf(rows, options);
    const int last = rowsOf(rows, options);
    const QString null = options.nullText.isEmpty() ? QStringLiteral("[NULL]") : options.nullText;

    auto cell = [&](int row, int column) {
        return cut(rows.isNull(row, column) ? null : oneLine(text(rows, row, column)),
                   options.maxCellWidth);
    };

    // Column widths first: the whole table has to line up.
    std::vector<int> width(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i)
        width[i] = int(cut(rows.column(columns[i]).name, options.maxCellWidth).size());
    for (int row = 0; row < last; ++row)
        for (std::size_t i = 0; i < columns.size(); ++i)
            width[i] = std::max(width[i], int(cell(row, columns[i]).size()));

    auto pad = [](const QString &value, int to, bool right) {
        const QString spaces(std::max(0, to - int(value.size())), QLatin1Char(' '));
        return right ? spaces + value : value + spaces;
    };

    // The last column is not padded: trailing spaces are of no use in a file.
    const std::size_t lastColumn = columns.size() - 1;
    if (options.header) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const QString name = cut(rows.column(columns[i]).name, options.maxCellWidth);
            out << (i ? QStringLiteral(" | ") : QString())
                << (i == lastColumn ? name : pad(name, width[i], false));
        }
        out << '\n';
        for (std::size_t i = 0; i < columns.size(); ++i)
            out << (i ? QStringLiteral("-+-") : QString()) << QString(width[i], QLatin1Char('-'));
        out << '\n';
    }
    for (int row = 0; row < last; ++row) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const int column = columns[i];
            const bool number
                = isNumericType(rows.column(column).type) && !rows.isNull(row, column);
            const QString value = cell(row, column);
            out << (i ? QStringLiteral(" | ") : QString())
                << (i == lastColumn && !number ? value : pad(value, width[i], number));
        }
        out << '\n';
    }
    if (const int more = rows.rowCount() - last; more > 0)
        out << (more == 1
                    ? QCoreApplication::translate("slonisko::catalog", "… 1 more row")
                    : QCoreApplication::translate("slonisko::catalog", "… %1 more rows").arg(more))
            << '\n';
}

QString literal(const pg::RowStore &rows, int row, int column)
{
    if (rows.isNull(row, column))
        return QStringLiteral("NULL");
    const QString value = text(rows, row, column);
    const Oid type = rows.column(column).type;
    if (isNumericType(type))
        return value;
    if (type == 16) // bool: t and f are no good in an INSERT.
        return value == QLatin1String("t") ? QStringLiteral("true") : QStringLiteral("false");
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

void writeInserts(const pg::RowStore &rows, const ExportOptions &options, QTextStream &out)
{
    const std::vector<int> columns = columnsOf(rows, options);
    if (columns.empty())
        return;
    QStringList names;
    for (const int column : columns)
        names << scope::quoted(rows.column(column).name);
    const QString into = QStringLiteral("INSERT INTO %1 (%2) VALUES")
                             .arg(options.table, names.join(QStringLiteral(", ")));

    const int last = rowsOf(rows, options);
    const bool bulk = options.format == ExportFormat::SqlBulkInsert;
    const int perStatement = bulk ? std::max(1, options.bulkRows) : 1;
    for (int row = 0; row < last; ++row) {
        QStringList values;
        for (const int column : columns)
            values << literal(rows, row, column);
        const QString tuple
            = QLatin1Char('(') + values.join(QStringLiteral(", ")) + QLatin1Char(')');
        const bool first = row % perStatement == 0;
        const bool lastOfStatement = row + 1 == last || (row + 1) % perStatement == 0;
        if (first)
            out << into << (bulk ? QLatin1Char('\n') : QLatin1Char(' '));
        out << (bulk ? QStringLiteral("    ") : QString()) << tuple
            << (lastOfStatement ? QStringLiteral(";\n") : QStringLiteral(",\n"));
    }
}

} // namespace

bool isNumericType(Oid type)
{
    switch (type) {
    case 20: // int8
    case 21: // int2
    case 23: // int4
    case 26: // oid
    case 700: // float4
    case 701: // float8
    case 1700: // numeric
        return true;
    default:
        return false;
    }
}

void exportRows(const pg::RowStore &rows, const ExportOptions &options, QTextStream &out)
{
    if (!rows.hasColumns())
        return;
    switch (options.format) {
    case ExportFormat::Csv:
        writeSeparated(rows, options, options.delimiter, out);
        return;
    case ExportFormat::Tsv:
        writeSeparated(rows, options, QLatin1Char('\t'), out);
        return;
    case ExportFormat::Text:
        writeText(rows, options, out);
        return;
    case ExportFormat::SqlInsert:
    case ExportFormat::SqlBulkInsert:
        writeInserts(rows, options, out);
        return;
    }
}

QString exportRows(const pg::RowStore &rows, const ExportOptions &options)
{
    QString out;
    QTextStream stream(&out);
    exportRows(rows, options, stream);
    stream.flush();
    return out;
}

QString formatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Csv:
        return QCoreApplication::translate("slonisko::catalog", "CSV");
    case ExportFormat::Tsv:
        return QCoreApplication::translate("slonisko::catalog", "Tab-separated");
    case ExportFormat::Text:
        return QCoreApplication::translate("slonisko::catalog", "Text table");
    case ExportFormat::SqlInsert:
        return QCoreApplication::translate("slonisko::catalog", "SQL INSERT");
    case ExportFormat::SqlBulkInsert:
        return QCoreApplication::translate("slonisko::catalog", "SQL bulk INSERT");
    }
    return {};
}

QString fileSuffix(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Csv:
        return QStringLiteral("csv");
    case ExportFormat::Tsv:
        return QStringLiteral("tsv");
    case ExportFormat::Text:
        return QStringLiteral("txt");
    case ExportFormat::SqlInsert:
    case ExportFormat::SqlBulkInsert:
        return QStringLiteral("sql");
    }
    return QStringLiteral("txt");
}

QString fileFilter(ExportFormat format)
{
    return QStringLiteral("%1 (*.%2)").arg(formatName(format), fileSuffix(format));
}

} // namespace slonisko::catalog
