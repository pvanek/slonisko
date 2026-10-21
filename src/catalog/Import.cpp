// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Import.h"

#include "catalog/Export.h"
#include "catalog/Scope.h"
#include "pg/Connection.h"
#include "pg/Result.h"

#include <QCoreApplication>
#include <QDate>
#include <QFile>
#include <QRegularExpression>

#include <limits>

namespace slonisko::catalog {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("slonisko::catalog", text);
}

QString qualified(const QString &schema, const QString &table)
{
    return schema.isEmpty() ? scope::quoted(table)
                            : scope::quoted(schema) + QLatin1Char('.') + scope::quoted(table);
}

constexpr qint64 BlockSize = 64 * 1024;

} // namespace

// CsvParser.

CsvParser::CsvParser(const CsvOptions &options)
    : m_options(options), m_toSkip(options.skipLines) { }

void CsvParser::feed(QStringView text)
{
    const QChar quote = m_options.quote;
    const QChar delimiter = m_options.delimiter;
    for (const QChar c : text) {
        if (m_toSkip > 0) {
            if (c == QLatin1Char('\n'))
                --m_toSkip;
            continue;
        }
        if (m_afterCr) {
            m_afterCr = false;
            if (c == QLatin1Char('\n'))
                continue;
        }
        if (m_inQuotes) {
            if (!m_quoteSeen) {
                if (c == quote)
                    m_quoteSeen = true;
                else
                    m_field += c;
                continue;
            }
            m_quoteSeen = false;
            if (c == quote) { // Doubled: a quote of the value's own.
                m_field += c;
                continue;
            }
            m_inQuotes = false; // The quote before closed the field.
        }
        if (c == delimiter) {
            endField();
            m_recordStarted = true;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            // Blank lines are no records: a file often ends with one or two.
            if (m_recordStarted || !m_field.isEmpty())
                endRecord();
            m_afterCr = c == QLatin1Char('\r');
        } else if (c == quote && m_field.isEmpty() && !m_quoted) {
            m_inQuotes = true;
            m_quoted = true;
            m_recordStarted = true;
        } else {
            m_field += c;
            m_recordStarted = true;
        }
    }
}

void CsvParser::finish()
{
    if (m_inQuotes && !m_quoteSeen)
        m_unterminated = true;
    m_inQuotes = false;
    m_quoteSeen = false;
    if (m_recordStarted || !m_field.isEmpty())
        endRecord();
}

std::vector<CsvRecord> CsvParser::take()
{
    return std::exchange(m_done, {});
}

void CsvParser::endField()
{
    if (!m_quoted && m_field == m_options.nullText)
        m_record.fields.emplace_back(std::nullopt);
    else
        m_record.fields.emplace_back(m_field);
    m_field.clear();
    m_quoted = false;
}

void CsvParser::endRecord()
{
    endField();
    m_done.push_back(std::move(m_record));
    m_record = {};
    m_recordStarted = false;
}

// CsvFile.

CsvFile::CsvFile(QIODevice *device, const CsvOptions &options)
    : m_device(device), m_options(options), m_decoder(options.encoding.constData()),
      m_parser(options), m_headerRead(!options.header)
{
    if (!m_decoder.isValid())
        m_decoder = QStringDecoder(QStringDecoder::Utf8);
}

CsvFile::~CsvFile() = default;

void CsvFile::fill()
{
    const QByteArray bytes = m_device->read(BlockSize);
    if (bytes.isEmpty()) {
        m_parser.feed(QString(m_decoder.decode(QByteArrayView())));
        m_parser.finish();
        m_finished = true;
        if (m_parser.unterminated())
            m_error = tr("The file ends inside a quoted field: a quote is not closed.");
    } else {
        m_parser.feed(QString(m_decoder.decode(bytes)));
    }
    for (CsvRecord &record : m_parser.take())
        m_pending.push_back(std::move(record));
}

std::vector<CsvRecord> CsvFile::read(int count)
{
    while (!m_headerRead) {
        if (m_next < m_pending.size()) {
            for (const std::optional<QString> &name : m_pending[m_next].fields)
                m_header << name.value_or(QString()).trimmed();
            ++m_next;
            m_headerRead = true;
        } else if (m_finished) {
            m_headerRead = true;
        } else {
            fill();
        }
    }
    std::vector<CsvRecord> out;
    while (int(out.size()) < count) {
        if (m_next < m_pending.size()) {
            out.push_back(std::move(m_pending[m_next++]));
            continue;
        }
        m_pending.clear();
        m_next = 0;
        if (m_finished)
            break;
        fill();
    }
    return out;
}

bool CsvFile::atEnd() const
{
    return m_finished && m_headerRead && m_next >= m_pending.size();
}

qint64 CsvFile::position() const
{
    return m_device->pos();
}

// Guessing.

QChar guessDelimiter(QStringView sample)
{
    // The last line may be cut off in the middle.
    const qsizetype lastBreak = sample.lastIndexOf(QLatin1Char('\n'));
    const QStringView whole = lastBreak > 0 ? sample.left(lastBreak + 1) : sample;
    QChar best = QLatin1Char(',');
    std::size_t bestFields = 1;
    for (const QChar candidate :
         {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char('|')}) {
        CsvOptions options;
        options.delimiter = candidate;
        CsvParser parser(options);
        parser.feed(whole);
        parser.finish();
        const std::vector<CsvRecord> records = parser.take();
        if (records.empty())
            continue;
        const std::size_t fields = records.front().fields.size();
        const bool even = std::ranges::all_of(
            records, [fields](const CsvRecord &r) { return r.fields.size() == fields; });
        if (even && fields > bestFields) {
            best = candidate;
            bestFields = fields;
        }
    }
    return best;
}

QString guessType(const std::vector<std::optional<QString>> &values)
{
    static const QRegularExpression integer(QStringLiteral("^[+-]?\\d+$"));
    static const QRegularExpression number(
        QStringLiteral("^[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?$"));
    static const QRegularExpression timestamp(QStringLiteral(
        "^(\\d{4}-\\d{2}-\\d{2})[ T]\\d{2}:\\d{2}(:\\d{2}(\\.\\d+)?)?(Z|[+-]\\d{2}(:?\\d{2})?)?$"));
    static const QStringList booleans {QStringLiteral("true"), QStringLiteral("false"),
                                       QStringLiteral("t"),    QStringLiteral("f"),
                                       QStringLiteral("yes"),  QStringLiteral("no")};

    bool any = false;
    bool int4 = true, int8 = true, numeric = true, boolean = true, date = true, stamp = true;
    bool zone = false;
    for (const std::optional<QString> &value : values) {
        if (!value)
            continue;
        any = true;
        const QString v = value->trimmed();
        if (integer.match(v).hasMatch()) {
            bool ok = false;
            const qlonglong n = v.toLongLong(&ok);
            int8 = int8 && ok;
            int4 = int4 && ok && n >= std::numeric_limits<qint32>::min()
                && n <= std::numeric_limits<qint32>::max();
        } else {
            int4 = int8 = false;
        }
        numeric = numeric && number.match(v).hasMatch();
        boolean = boolean && booleans.contains(v, Qt::CaseInsensitive);
        date = date && v.size() == 10 && QDate::fromString(v, Qt::ISODate).isValid();
        const QRegularExpressionMatch m = timestamp.match(v);
        stamp = stamp && m.hasMatch() && QDate::fromString(m.captured(1), Qt::ISODate).isValid();
        zone = zone || !m.captured(4).isEmpty();
    }
    if (!any)
        return QStringLiteral("text");
    if (int4)
        return QStringLiteral("integer");
    if (int8)
        return QStringLiteral("bigint");
    if (numeric)
        return QStringLiteral("numeric");
    if (boolean)
        return QStringLiteral("boolean");
    if (date)
        return QStringLiteral("date");
    if (stamp)
        return zone ? QStringLiteral("timestamptz") : QStringLiteral("timestamp");
    return QStringLiteral("text");
}

// Statements.

QByteArray copyRows(const std::vector<CsvRecord> &records, const std::vector<int> &source)
{
    QString out;
    for (const CsvRecord &record : records) {
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (i)
                out += QLatin1Char('\t');
            const auto field = std::size_t(source[i]);
            if (field < record.fields.size() && record.fields[field])
                out += copyEscaped(*record.fields[field]);
            else
                out += QLatin1String("\\N");
        }
        out += QLatin1Char('\n');
    }
    return out.toUtf8();
}

QByteArray copyStatement(const QString &schema, const QString &table, const QStringList &columns)
{
    QStringList names;
    for (const QString &column : columns)
        names << scope::quoted(column);
    return QStringLiteral("COPY %1 (%2) FROM STDIN")
        .arg(qualified(schema, table), names.join(QStringLiteral(", ")))
        .toUtf8();
}

QByteArray createTableStatement(const ImportPlan &plan)
{
    QStringList columns;
    for (const ImportColumn &column : plan.columns)
        columns << QStringLiteral("    %1 %2").arg(scope::quoted(column.name), column.type);
    return QStringLiteral("CREATE TABLE %1 (\n%2\n)")
        .arg(qualified(plan.schema, plan.table), columns.join(QStringLiteral(",\n")))
        .toUtf8();
}

QByteArray tableColumnsQuery(const QString &schema, const QString &table)
{
    QString name = qualified(schema, table);
    name.replace(QLatin1Char('\''), QLatin1String("''"));
    return QStringLiteral("SELECT a.attname, format_type(a.atttypid, a.atttypmod)\n"
                          "FROM pg_catalog.pg_attribute a\n"
                          "WHERE a.attrelid = '%1'::regclass AND a.attnum > 0\n"
                          "    AND NOT a.attisdropped AND a.attgenerated = ''\n"
                          "ORDER BY a.attnum")
        .arg(name)
        .toUtf8();
}

// ImportJob.

ImportJob::ImportJob(pg::Connection *connection, ImportPlan plan, QObject *parent)
    : QObject(parent), m_connection(connection), m_plan(std::move(plan))
{ }

ImportJob::~ImportJob()
{
    for (const QMetaObject::Connection &c : std::as_const(m_signals))
        disconnect(c);
}

void ImportJob::start()
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_error.clear();
    m_rows = 0;

    auto file = std::make_unique<QFile>(m_plan.path);
    if (!file->open(QIODevice::ReadOnly)) {
        done(false, tr("Could not read %1: %2").arg(m_plan.path, file->errorString()));
        return;
    }
    m_file = std::move(file);
    m_csv = std::make_unique<CsvFile>(m_file.get(), m_plan.csv);
    if (!m_connection || m_connection->state() != pg::Connection::State::Ready) {
        done(false, tr("The connection is not ready."));
        return;
    }
    m_signals << connect(m_connection, &pg::Connection::resultReady, this, &ImportJob::onResult)
              << connect(m_connection, &pg::Connection::queryFinished, this, &ImportJob::onFinished)
              << connect(m_connection, &pg::Connection::stateChanged, this,
                         [this](pg::Connection::State state) {
                             if (state == pg::Connection::State::Failed)
                                 done(false, m_connection->errorMessage());
                             else if (state == pg::Connection::State::Disconnected)
                                 done(false, tr("The connection was closed."));
                         });

    QByteArray sql = "BEGIN";
    if (m_plan.create)
        sql += ";\n" + createTableStatement(m_plan);
    if (m_plan.truncate && !m_plan.create)
        sql += ";\nTRUNCATE " + qualified(m_plan.schema, m_plan.table).toUtf8();
    run(Step::Begin, sql);
}

void ImportJob::cancel()
{
    if (!m_running || m_cancelled)
        return;
    m_cancelled = true;
    // The running statement ends with an error, and the rollback follows.
    if (m_connection && m_step != Step::Rollback)
        m_connection->cancel();
}

void ImportJob::run(Step step, const QByteArray &sql, std::optional<QByteArray> data)
{
    m_step = step;
    if (!m_connection || !m_connection->execute(sql, 0, std::move(data)))
        done(false, tr("The connection is not ready."));
}

void ImportJob::onResult(const pg::Result &result)
{
    if (!result.isError() || !m_error.isEmpty() || m_step == Step::Rollback)
        return;
    const QString message
        = QString::fromUtf8(PQresultErrorField(result.get(), PG_DIAG_MESSAGE_PRIMARY));
    if (m_step != Step::Copy) {
        m_error = message;
        return;
    }
    // "COPY t, line 3, column x: ..." and the like in the server's language:
    // the line counts the rows of the batch.
    static const QRegularExpression line(QStringLiteral("^COPY [^\\n]*?, \\D*(\\d+)"));
    const QString context = QString::fromUtf8(PQresultErrorField(result.get(), PG_DIAG_CONTEXT));
    if (const QRegularExpressionMatch m = line.match(context); m.hasMatch()) {
        m_error = tr("Row %1 of the file's data: %2")
                      .arg(m_batchStart + m.captured(1).toLongLong())
                      .arg(message);
    } else {
        m_error = tr("Rows %1 to %2 of the file's data: %3")
                      .arg(m_batchStart + 1)
                      .arg(m_batchStart + m_batchSize)
                      .arg(message);
    }
}

void ImportJob::onFinished()
{
    if (!m_running)
        return;
    if (m_step == Step::Rollback) {
        done(false,
             m_cancelled && m_error.isEmpty()
                 ? tr("The import was stopped; nothing was imported.")
                 : m_error + QLatin1Char('\n') + tr("Nothing was imported."));
        return;
    }
    if (!m_error.isEmpty() || m_cancelled) {
        // A cancel that came too late to stop a statement still undoes it,
        // once it has arrived: before, it could hit the rollback instead.
        if (m_cancelled && m_connection->isCancelling()) {
            m_signals << connect(m_connection, &pg::Connection::cancelDone, this,
                                 &ImportJob::onFinished, Qt::SingleShotConnection);
            return;
        }
        if (m_cancelled)
            m_error.clear();
        run(Step::Rollback, "ROLLBACK");
        return;
    }
    switch (m_step) {
    case Step::Begin:
        sendBatch();
        break;
    case Step::Copy:
        m_rows += m_batchSize;
        Q_EMIT progress(m_csv->position(), m_file->size(), m_rows);
        sendBatch();
        break;
    case Step::Commit:
        done(true,
             tr("Imported %1 rows into %2.")
                 .arg(m_rows)
                 .arg(qualified(m_plan.schema, m_plan.table)));
        break;
    case Step::Rollback:
        break;
    }
}

void ImportJob::sendBatch()
{
    const std::vector<CsvRecord> records = m_csv->read(std::max(1, m_plan.batchRows));
    if (!m_csv->error().isEmpty()) {
        fail(m_csv->error());
        return;
    }
    if (records.empty()) {
        run(Step::Commit, "COMMIT");
        return;
    }
    std::vector<int> source;
    QStringList names;
    for (const ImportColumn &column : m_plan.columns) {
        source.push_back(column.source);
        names << column.name;
    }
    m_batchStart = m_rows;
    m_batchSize = qint64(records.size());
    run(Step::Copy, copyStatement(m_plan.schema, m_plan.table, names), copyRows(records, source));
}

void ImportJob::fail(const QString &message)
{
    m_error = message;
    run(Step::Rollback, "ROLLBACK");
}

void ImportJob::done(bool ok, const QString &message)
{
    if (!m_running)
        return;
    m_running = false;
    for (const QMetaObject::Connection &c : std::as_const(m_signals))
        disconnect(c);
    m_signals.clear();
    m_csv.reset();
    m_file.reset();
    Q_EMIT finished(ok, message);
}

} // namespace slonisko::catalog
