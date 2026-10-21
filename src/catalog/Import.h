// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

class QIODevice;

namespace slonisko::pg {
class Connection;
class Result;
} // namespace slonisko::pg

namespace slonisko::catalog {

struct CsvOptions
{
    QChar delimiter = QLatin1Char(',');
    QChar quote = QLatin1Char('"');
    bool header = true; // The first record names the columns.
    int skipLines = 0; // Lines before the header, like a title or a note.
    // An unquoted field that reads exactly this is NULL; a quoted one never
    // is, so "" stays an empty string. The same as COPY's CSV format.
    QString nullText;
    QByteArray encoding = "UTF-8";
};

// One line of the file, or more when a quoted field spans lines.
struct CsvRecord
{
    std::vector<std::optional<QString>> fields; // nullopt for NULL.
};

// Parses CSV text as it arrives, in pieces of any size: RFC 4180, with the
// delimiter and the quote of one's choice, and line breaks inside quotes.
class CsvParser
{
public:
    explicit CsvParser(const CsvOptions &options);

    void feed(QStringView text);
    // The end of the input: a last record with no line break after it.
    void finish();
    std::vector<CsvRecord> take();
    // A quote that was never closed, found by finish().
    bool unterminated() const { return m_unterminated; }

private:
    void endField();
    void endRecord();

    CsvOptions m_options;
    int m_toSkip = 0;
    bool m_inQuotes = false;
    bool m_quoteSeen = false; // In quotes, the last character was a quote.
    bool m_quoted = false; // The current field was quoted.
    bool m_afterCr = false; // A \r ended the last record; a \n may follow.
    bool m_unterminated = false;
    QString m_field;
    CsvRecord m_record;
    bool m_recordStarted = false;
    std::vector<CsvRecord> m_done;
};

// A CSV file read a few records at a time, so that a big one is never in
// memory whole.
class CsvFile
{
public:
    CsvFile(QIODevice *device, const CsvOptions &options);
    ~CsvFile();

    // Up to count data records; fewer only at the end of the file.
    std::vector<CsvRecord> read(int count);
    bool atEnd() const;
    // The header's names once read() has run, if options.header.
    QStringList header() const { return m_header; }
    // Bytes read so far, for a progress bar.
    qint64 position() const;
    // Set when the file ends inside a quoted field.
    QString error() const { return m_error; }

private:
    void fill();

    QIODevice *m_device;
    CsvOptions m_options;
    QStringDecoder m_decoder;
    CsvParser m_parser;
    std::vector<CsvRecord> m_pending;
    std::size_t m_next = 0;
    bool m_headerRead = false;
    bool m_finished = false;
    QStringList m_header;
    QString m_error;
};

// The delimiter that splits the sample's lines into the same number of
// fields, the most of them; a comma when none does.
QChar guessDelimiter(QStringView sample);
// A column type that fits every value: integer, bigint, numeric, boolean,
// date, timestamp, timestamptz, or text. NULLs fit anything.
QString guessType(const std::vector<std::optional<QString>> &values);

// Rows for COPY ... FROM STDIN in its text format. Each target column takes
// the record's field at source[i]; a record too short for it gives NULL.
QByteArray copyRows(const std::vector<CsvRecord> &records, const std::vector<int> &source);
QByteArray copyStatement(const QString &schema, const QString &table, const QStringList &columns);

struct ImportColumn
{
    int source = 0; // The field in each record.
    QString name; // The table's column.
    QString type; // Only for a table created for the import.
};

struct ImportPlan
{
    QString path;
    CsvOptions csv;
    QString schema;
    QString table;
    std::vector<ImportColumn> columns;
    bool create = false; // CREATE TABLE first, with the columns' types.
    bool truncate = false; // Empty the table first.
    int batchRows = 5000; // Records per COPY.
};

QByteArray createTableStatement(const ImportPlan &plan);

// The columns an import can fill: name and type, in the table's order.
// Generated columns are left out, as COPY cannot write them.
QByteArray tableColumnsQuery(const QString &schema, const QString &table);

// Imports a CSV file into a table over a connection of its own, in one
// transaction: the rows go in as COPYs of a few thousand at a time, so a
// file of any size takes little memory, shows progress and can be stopped.
// If anything fails, nothing is imported.
class ImportJob : public QObject
{
    Q_OBJECT

public:
    // connection must be ready and stay alive until finished().
    ImportJob(pg::Connection *connection, ImportPlan plan, QObject *parent = nullptr);
    ~ImportJob() override;

    void start();
    // Stops the import and rolls it back; finished() follows.
    void cancel();
    bool isRunning() const { return m_running; }
    qint64 rows() const { return m_rows; }

Q_SIGNALS:
    void progress(qint64 bytesRead, qint64 bytesTotal, qint64 rows);
    // message says what was done, or why nothing was.
    void finished(bool ok, const QString &message);

private:
    enum class Step { Begin, Copy, Commit, Rollback };

    void run(Step step, const QByteArray &sql, std::optional<QByteArray> data = std::nullopt);
    void onResult(const pg::Result &result);
    void onFinished();
    void sendBatch();
    void fail(const QString &message);
    void done(bool ok, const QString &message);

    QPointer<pg::Connection> m_connection;
    ImportPlan m_plan;
    std::unique_ptr<QIODevice> m_file;
    std::unique_ptr<CsvFile> m_csv;
    QList<QMetaObject::Connection> m_signals;
    Step m_step = Step::Begin;
    bool m_running = false;
    bool m_cancelled = false;
    QString m_error;
    qint64 m_rows = 0; // Sent and accepted.
    qint64 m_batchStart = 0; // The first record of the batch running, from 0.
    qint64 m_batchSize = 0;
};

} // namespace slonisko::catalog
