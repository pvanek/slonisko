// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Export.h"

#include "catalog/Scope.h"

#include <QCoreApplication>
#include <QTextStream>

#include <algorithm>

namespace slonisko::catalog {

namespace {

constexpr Oid BoolType = 16;
constexpr Oid JsonType = 114;
constexpr Oid JsonbType = 3802;

std::vector<int> columnsOf(const pg::RowStore &rows, const ExportOptions &options)
{
    if (!options.columns.empty())
        return options.columns;
    std::vector<int> all(std::size_t(rows.columnCount()));
    for (int c = 0; c < rows.columnCount(); ++c)
        all[std::size_t(c)] = c;
    return all;
}

// The rows to write, in order: the chosen ones, or all of them, and never
// more than maxRows.
std::vector<int> rowsOf(const pg::RowStore &rows, const ExportOptions &options)
{
    std::vector<int> out;
    if (options.rows.empty()) {
        const int last
            = options.maxRows > 0 ? std::min(options.maxRows, rows.rowCount()) : rows.rowCount();
        out.resize(std::size_t(std::max(0, last)));
        for (int row = 0; row < last; ++row)
            out[std::size_t(row)] = row;
        return out;
    }
    for (const int row : options.rows) {
        if (row < 0 || row >= rows.rowCount())
            continue; // A row the user added and has not saved yet.
        if (options.maxRows > 0 && int(out.size()) >= options.maxRows)
            break;
        out.push_back(row);
    }
    return out;
}

// How many rows were left out, for the text view's "… more rows" line.
int skipped(const pg::RowStore &rows, const ExportOptions &options, std::size_t written)
{
    const int total = options.rows.empty() ? rows.rowCount() : int(options.rows.size());
    return total - int(written);
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

QString literal(const pg::RowStore &rows, int row, int column)
{
    if (rows.isNull(row, column))
        return QStringLiteral("NULL");
    const QString value = text(rows, row, column);
    const Oid type = rows.column(column).type;
    if (isNumericType(type))
        return value;
    if (type == BoolType) // t and f are no good in an INSERT.
        return value == QLatin1String("t") ? QStringLiteral("true") : QStringLiteral("false");
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString jsonString(const QString &value)
{
    QString out;
    out.reserve(value.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : value) {
        switch (c.unicode()) {
        case '"':
            out += QLatin1String("\\\"");
            break;
        case '\\':
            out += QLatin1String("\\\\");
            break;
        case '\n':
            out += QLatin1String("\\n");
            break;
        case '\r':
            out += QLatin1String("\\r");
            break;
        case '\t':
            out += QLatin1String("\\t");
            break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    out += QLatin1Char('"');
    return out;
}

QString jsonValue(const pg::RowStore &rows, int row, int column)
{
    if (rows.isNull(row, column))
        return QStringLiteral("null");
    const QString value = text(rows, row, column);
    const Oid type = rows.column(column).type;
    // NaN and Infinity are numbers to PostgreSQL but not to JSON.
    if (isNumericType(type) && !value.isEmpty()
        && (value.front().isDigit() || value.front() == QLatin1Char('-')) && value.back().isDigit())
        return value;
    if (type == BoolType)
        return value == QLatin1String("t") ? QStringLiteral("true") : QStringLiteral("false");
    // The server wrote it, so it is JSON already.
    if (type == JsonType || type == JsonbType)
        return value;
    return jsonString(value);
}

QString markdownCell(const QString &value)
{
    QString out = value;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('|'), QLatin1String("\\|"));
    out.replace(QLatin1String("\r\n"), QLatin1String("<br>"));
    out.replace(QLatin1Char('\n'), QLatin1String("<br>"));
    out.replace(QLatin1Char('\r'), QLatin1String("<br>"));
    return out;
}

// XML 1.0 has no way at all to write most control characters.
QString xmlText(const QString &value)
{
    QString out;
    out.reserve(value.size());
    for (const QChar c : value) {
        switch (c.unicode()) {
        case '&':
            out += QLatin1String("&amp;");
            break;
        case '<':
            out += QLatin1String("&lt;");
            break;
        case '>':
            out += QLatin1String("&gt;");
            break;
        case '"':
            out += QLatin1String("&quot;");
            break;
        case '\t':
        case '\n':
            out += c;
            break;
        case '\r':
            out += QLatin1String("&#13;"); // A parser would turn a bare one into \n.
            break;
        default:
            out += c.unicode() < 0x20 ? QChar(QChar::ReplacementCharacter) : c;
        }
    }
    return out;
}

} // namespace

struct RowWriter::State
{
    State(const ExportOptions &chosen, QTextStream &stream) : options(chosen), out(stream) { }

    ExportOptions options;
    QTextStream &out;
    bool started = false;
    bool finished = false;
    int written = 0;
    // From the first part: the rows of later ones are in the same columns.
    std::vector<int> columns;
    std::vector<QString> names;
    std::vector<bool> numeric;
    std::vector<int> width; // Text: fixed by the first part.

    void begin(const pg::RowStore &rows);
    void row(const pg::RowStore &rows, int row);
    void end();
    void writeText(const pg::RowStore &rows, const std::vector<int> &wanted);
    QString columnList() const;
    QString cell(const pg::RowStore &rows, int row, int column, const QString &null) const
    {
        return rows.isNull(row, column) ? null : text(rows, row, column);
    }
};

QString RowWriter::State::columnList() const
{
    QStringList quoted;
    for (const QString &name : names)
        quoted << scope::quoted(name);
    return quoted.join(QStringLiteral(", "));
}

void RowWriter::State::begin(const pg::RowStore &rows)
{
    columns = columnsOf(rows, options);
    for (const int column : columns) {
        names.push_back(rows.column(column).name);
        numeric.push_back(isNumericType(rows.column(column).type));
    }
    const qsizetype count = qsizetype(columns.size());

    switch (options.format) {
    case ExportFormat::Csv:
    case ExportFormat::Tsv: {
        if (!options.header)
            break;
        const QChar delimiter
            = options.format == ExportFormat::Tsv ? QLatin1Char('\t') : options.delimiter;
        for (qsizetype i = 0; i < count; ++i)
            out << (i ? QString(delimiter) : QString()) << csvField(names[i], delimiter);
        out << '\n';
        break;
    }
    case ExportFormat::Text:
        break; // Its header needs the widths, which need the rows.
    case ExportFormat::Markdown: {
        // A pipe table cannot do without its header.
        out << '|';
        for (const QString &name : names)
            out << ' ' << markdownCell(name) << " |";
        out << "\n|";
        for (const bool number : numeric)
            out << (number ? "---:|" : "---|");
        out << '\n';
        break;
    }
    case ExportFormat::Html:
        out << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>"
            << xmlText(QCoreApplication::translate("slonisko::catalog", "Query result"))
            << "</title>\n"
            << "<style>\n"
               "table { border-collapse: collapse; }\n"
               "th, td { border: 1px solid #ccc; padding: 2px 6px; vertical-align: top; "
               "white-space: pre-wrap; }\n"
               "td.number { text-align: right; }\n"
               "</style>\n"
               "</head>\n<body>\n<table>\n";
        if (options.header) {
            out << "<thead>\n<tr>";
            for (const QString &name : names)
                out << "<th>" << xmlText(name) << "</th>";
            out << "</tr>\n</thead>\n";
        }
        out << "<tbody>\n";
        break;
    case ExportFormat::Json:
        out << '[';
        break;
    case ExportFormat::Xml:
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n";
        break;
    case ExportFormat::SqlCopy:
        out << "COPY " << options.table << " (" << columnList() << ") FROM stdin;\n";
        break;
    case ExportFormat::JsonLines:
    case ExportFormat::SqlInsert:
    case ExportFormat::SqlBulkInsert:
        break;
    }
}

void RowWriter::State::row(const pg::RowStore &rows, int row)
{
    const qsizetype count = qsizetype(columns.size());
    switch (options.format) {
    case ExportFormat::Csv:
    case ExportFormat::Tsv: {
        const QChar delimiter
            = options.format == ExportFormat::Tsv ? QLatin1Char('\t') : options.delimiter;
        for (qsizetype i = 0; i < count; ++i) {
            const int column = columns[std::size_t(i)];
            if (i)
                out << delimiter;
            out << (rows.isNull(row, column) ? options.nullText
                                             : csvField(text(rows, row, column), delimiter));
        }
        out << '\n';
        break;
    }
    case ExportFormat::Markdown:
        out << '|';
        for (const int column : columns)
            out << ' ' << markdownCell(cell(rows, row, column, options.nullText)) << " |";
        out << '\n';
        break;
    case ExportFormat::Html:
        out << "<tr>";
        for (qsizetype i = 0; i < count; ++i) {
            const int column = columns[std::size_t(i)];
            out << (numeric[std::size_t(i)] ? "<td class=\"number\">" : "<td>")
                << xmlText(cell(rows, row, column, options.nullText)) << "</td>";
        }
        out << "</tr>\n";
        break;
    case ExportFormat::Json:
    case ExportFormat::JsonLines: {
        const bool array = options.format == ExportFormat::Json;
        if (array)
            out << (written ? ",\n  " : "\n  ");
        out << '{';
        for (qsizetype i = 0; i < count; ++i) {
            out << (i ? ", " : "") << jsonString(names[std::size_t(i)]) << ": "
                << jsonValue(rows, row, columns[std::size_t(i)]);
        }
        out << '}';
        if (!array)
            out << '\n';
        break;
    }
    case ExportFormat::Xml:
        out << "  <row>\n";
        for (qsizetype i = 0; i < count; ++i) {
            const int column = columns[std::size_t(i)];
            out << "    <column name=\"" << xmlText(names[std::size_t(i)]) << '"';
            if (rows.isNull(row, column))
                out << " null=\"true\"/>\n";
            else
                out << '>' << xmlText(text(rows, row, column)) << "</column>\n";
        }
        out << "  </row>\n";
        break;
    case ExportFormat::SqlInsert:
    case ExportFormat::SqlBulkInsert: {
        if (columns.empty())
            break;
        QStringList values;
        for (const int column : columns)
            values << literal(rows, row, column);
        const QString tuple
            = QLatin1Char('(') + values.join(QStringLiteral(", ")) + QLatin1Char(')');
        const QString into
            = QStringLiteral("INSERT INTO %1 (%2) VALUES").arg(options.table, columnList());
        if (options.format == ExportFormat::SqlInsert) {
            out << into << ' ' << tuple << ";\n";
            break;
        }
        // The statement before is closed when this row starts a new one, or
        // by end(): only then is it known whether more rows follow.
        const int perStatement = std::max(1, options.bulkRows);
        if (written % perStatement == 0)
            out << (written ? ";\n" : "") << into << '\n';
        else
            out << ",\n";
        out << "    " << tuple;
        break;
    }
    case ExportFormat::SqlCopy:
        for (qsizetype i = 0; i < count; ++i) {
            const int column = columns[std::size_t(i)];
            out << (i ? "\t" : "")
                << (rows.isNull(row, column) ? QStringLiteral("\\N")
                                             : copyEscaped(text(rows, row, column)));
        }
        out << '\n';
        break;
    case ExportFormat::Text:
        break; // writeText() does the whole part at once.
    }
    ++written;
}

void RowWriter::State::writeText(const pg::RowStore &rows, const std::vector<int> &wanted)
{
    const QString null = options.nullText.isEmpty() ? QStringLiteral("[NULL]") : options.nullText;
    auto cellText = [&](int row, int column) {
        return cut(rows.isNull(row, column) ? null : oneLine(text(rows, row, column)),
                   options.maxCellWidth);
    };
    auto pad = [](const QString &value, int to, bool right) {
        const QString spaces(std::max(0, to - int(value.size())), QLatin1Char(' '));
        return right ? spaces + value : value + spaces;
    };
    const std::size_t count = columns.size();
    // The last column is not padded: trailing spaces are of no use in a file.
    const std::size_t lastColumn = count - 1;

    // The whole table has to line up, so the widths come first; from the
    // first part alone when the rows stream in, as the header is out by then.
    if (width.empty()) {
        width.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            width[i] = int(cut(names[i], options.maxCellWidth).size());
        for (const int row : wanted)
            for (std::size_t i = 0; i < count; ++i)
                width[i] = std::max(width[i], int(cellText(row, columns[i]).size()));
        if (options.header) {
            for (std::size_t i = 0; i < count; ++i) {
                const QString name = cut(names[i], options.maxCellWidth);
                out << (i ? QStringLiteral(" | ") : QString())
                    << (i == lastColumn ? name : pad(name, width[i], false));
            }
            out << '\n';
            for (std::size_t i = 0; i < count; ++i)
                out << (i ? QStringLiteral("-+-") : QString())
                    << QString(width[i], QLatin1Char('-'));
            out << '\n';
        }
    }
    for (const int row : wanted) {
        for (std::size_t i = 0; i < count; ++i) {
            const int column = columns[i];
            const bool number = numeric[i] && !rows.isNull(row, column);
            const QString value = cellText(row, column);
            out << (i ? QStringLiteral(" | ") : QString())
                << (i == lastColumn && !number ? value : pad(value, width[i], number));
        }
        out << '\n';
        ++written;
    }
    if (const int more = skipped(rows, options, wanted.size()); more > 0)
        out << (more == 1
                    ? QCoreApplication::translate("slonisko::catalog", "… 1 more row")
                    : QCoreApplication::translate("slonisko::catalog", "… %1 more rows").arg(more))
            << '\n';
}

void RowWriter::State::end()
{
    switch (options.format) {
    case ExportFormat::Html:
        out << "</tbody>\n</table>\n</body>\n</html>\n";
        break;
    case ExportFormat::Json:
        out << (written ? "\n]\n" : "]\n");
        break;
    case ExportFormat::Xml:
        out << "</rows>\n";
        break;
    case ExportFormat::SqlBulkInsert:
        if (written)
            out << ";\n";
        break;
    case ExportFormat::SqlCopy:
        out << "\\.\n";
        break;
    case ExportFormat::Csv:
    case ExportFormat::Tsv:
    case ExportFormat::Text:
    case ExportFormat::Markdown:
    case ExportFormat::JsonLines:
    case ExportFormat::SqlInsert:
        break;
    }
}

RowWriter::RowWriter(const ExportOptions &options, QTextStream &out)
    : m_state(std::make_unique<State>(options, out))
{ }

RowWriter::~RowWriter() = default;

void RowWriter::write(const pg::RowStore &rows)
{
    State &s = *m_state;
    if (!rows.hasColumns() || s.finished)
        return;
    if (!s.started) {
        s.begin(rows);
        s.started = true;
    }
    const std::vector<int> wanted = rowsOf(rows, s.options);
    if (s.options.format == ExportFormat::Text) {
        if (!s.columns.empty())
            s.writeText(rows, wanted);
        return;
    }
    for (const int row : wanted)
        s.row(rows, row);
}

void RowWriter::finish()
{
    State &s = *m_state;
    if (s.started && !s.finished)
        s.end();
    s.finished = true;
}

int RowWriter::rowsWritten() const
{
    return m_state->written;
}

QString copyEscaped(const QString &value)
{
    QString out;
    out.reserve(value.size());
    for (const QChar c : value) {
        switch (c.unicode()) {
        case '\\':
            out += QLatin1String("\\\\");
            break;
        case '\t':
            out += QLatin1String("\\t");
            break;
        case '\n':
            out += QLatin1String("\\n");
            break;
        case '\r':
            out += QLatin1String("\\r");
            break;
        default:
            out += c;
        }
    }
    return out;
}

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
    RowWriter writer(options, out);
    writer.write(rows);
    writer.finish();
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
    case ExportFormat::Json:
        return QCoreApplication::translate("slonisko::catalog", "JSON");
    case ExportFormat::JsonLines:
        return QCoreApplication::translate("slonisko::catalog", "JSON Lines");
    case ExportFormat::Markdown:
        return QCoreApplication::translate("slonisko::catalog", "Markdown");
    case ExportFormat::Html:
        return QCoreApplication::translate("slonisko::catalog", "HTML");
    case ExportFormat::Xml:
        return QCoreApplication::translate("slonisko::catalog", "XML");
    case ExportFormat::SqlCopy:
        return QCoreApplication::translate("slonisko::catalog", "SQL COPY");
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
    case ExportFormat::SqlCopy:
        return QStringLiteral("sql");
    case ExportFormat::Json:
        return QStringLiteral("json");
    case ExportFormat::JsonLines:
        return QStringLiteral("jsonl");
    case ExportFormat::Markdown:
        return QStringLiteral("md");
    case ExportFormat::Html:
        return QStringLiteral("html");
    case ExportFormat::Xml:
        return QStringLiteral("xml");
    }
    return QStringLiteral("txt");
}

QString fileFilter(ExportFormat format)
{
    return QStringLiteral("%1 (*.%2)").arg(formatName(format), fileSuffix(format));
}

} // namespace slonisko::catalog
