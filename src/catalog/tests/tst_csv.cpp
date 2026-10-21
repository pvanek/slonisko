// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Import.h"

#include <QBuffer>
#include <QTest>

using namespace slonisko::catalog;

namespace {

using Fields = std::vector<std::optional<QString>>;

std::vector<CsvRecord> parse(const QString &text, const CsvOptions &options = {})
{
    CsvParser parser(options);
    parser.feed(text);
    parser.finish();
    return parser.take();
}

Fields field(std::initializer_list<const char *> values)
{
    Fields out;
    for (const char *v : values)
        out.emplace_back(v ? std::optional<QString>(QString::fromUtf8(v)) : std::nullopt);
    return out;
}

} // namespace

class TestCsv : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesQuotesAndBreaks()
    {
        const auto records = parse(QStringLiteral("a,\"b, c\",\"say \"\"hi\"\"\"\r\n"
                                                  "1,\"two\nlines\",\n"
                                                  "\n"
                                                  "x,\"\",y"));
        QCOMPARE(records.size(), std::size_t(3));
        QCOMPARE(records[0].fields, field({"a", "b, c", "say \"hi\""}));
        // An unquoted empty field is NULL, a quoted one an empty string.
        QCOMPARE(records[1].fields, field({"1", "two\nlines", nullptr}));
        QCOMPARE(records[2].fields, field({"x", "", "y"}));
    }

    // The file arrives in blocks that may cut anything in two, a doubled
    // quote and a \r\n included.
    void parsesInPieces()
    {
        const QString text = QStringLiteral("\"a\"\"b\",c\r\nd,e\r\n");
        for (qsizetype cut = 1; cut < text.size(); ++cut) {
            CsvParser parser({});
            parser.feed(QStringView(text).left(cut));
            parser.feed(QStringView(text).mid(cut));
            parser.finish();
            const auto records = parser.take();
            QCOMPARE(records.size(), std::size_t(2));
            QCOMPARE(records[0].fields, field({"a\"b", "c"}));
            QCOMPARE(records[1].fields, field({"d", "e"}));
        }
    }

    void options()
    {
        CsvOptions options;
        options.delimiter = QLatin1Char(';');
        options.quote = QLatin1Char('\'');
        options.nullText = QStringLiteral("NULL");
        options.skipLines = 1;
        const auto records
            = parse(QStringLiteral("Exported 2026-10-21\n'a;b';NULL;'NULL';\n"), options);
        QCOMPARE(records.size(), std::size_t(1));
        // With a NULL text, empty fields are empty strings.
        QCOMPARE(records[0].fields, field({"a;b", nullptr, "NULL", ""}));
    }

    void unterminatedQuote()
    {
        CsvParser parser({});
        parser.feed(QStringLiteral("a,\"b\n"));
        parser.finish();
        QVERIFY(parser.unterminated());
        QCOMPARE(parser.take().size(), std::size_t(1));
    }

    void readsAFile()
    {
        QByteArray bytes = "\xEF\xBB\xBF"
                           "id,name\n"; // With a byte order mark.
        for (int i = 1; i <= 10; ++i)
            bytes += QByteArray::number(i) + ",n\xC3\xA1zev " + QByteArray::number(i) + '\n';
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        CsvFile file(&buffer, {});
        const auto first = file.read(4);
        QCOMPARE(file.header(), (QStringList {QStringLiteral("id"), QStringLiteral("name")}));
        QCOMPARE(first.size(), std::size_t(4));
        QCOMPARE(first[0].fields, field({"1", "název 1"}));
        QVERIFY(!file.atEnd());
        QCOMPARE(file.read(100).size(), std::size_t(6));
        QVERIFY(file.atEnd());
        QVERIFY(file.read(100).empty());
        QVERIFY(file.error().isEmpty());
        QCOMPARE(file.position(), qint64(bytes.size()));
    }

    void readsOtherEncodings()
    {
        QByteArray bytes = "a\n\xE8\x61j\n"; // "čaj" in Windows-1250.
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        CsvOptions options;
        options.encoding = "windows-1250";
        CsvFile file(&buffer, options);
        const auto records = file.read(10);
        QCOMPARE(records.size(), std::size_t(1));
        QCOMPARE(records[0].fields, field({"čaj"}));
    }

    void guessesTheDelimiter()
    {
        QCOMPARE(guessDelimiter(u"a,b,c\n1,2,3\n"), QLatin1Char(','));
        QCOMPARE(guessDelimiter(u"a;b;c\n1,5;2;3\n4;5;6"), QLatin1Char(';'));
        QCOMPARE(guessDelimiter(u"a\tb\n\"x,y\"\t2\n"), QLatin1Char('\t'));
        QCOMPARE(guessDelimiter(u"a|b\n1|2\n"), QLatin1Char('|'));
        QCOMPARE(guessDelimiter(u"just text\n"), QLatin1Char(','));
    }

    void guessesTypes()
    {
        QCOMPARE(guessType(field({"1", "-20", nullptr})), QStringLiteral("integer"));
        QCOMPARE(guessType(field({"1", "9000000000"})), QStringLiteral("bigint"));
        QCOMPARE(guessType(field({"1", "2.5", "1e3"})), QStringLiteral("numeric"));
        QCOMPARE(guessType(field({"99999999999999999999"})), QStringLiteral("numeric"));
        QCOMPARE(guessType(field({"true", "F", "yes"})), QStringLiteral("boolean"));
        QCOMPARE(guessType(field({"2026-10-21", "2024-02-29"})), QStringLiteral("date"));
        QCOMPARE(guessType(field({"2026-02-30"})), QStringLiteral("text"));
        QCOMPARE(guessType(field({"2026-10-21 10:00", "2026-10-21T10:00:01.5"})),
                 QStringLiteral("timestamp"));
        QCOMPARE(guessType(field({"2026-10-21 10:00:00+02", "2026-10-21 08:00:00Z"})),
                 QStringLiteral("timestamptz"));
        // An empty string goes into no number.
        QCOMPARE(guessType(field({"1", ""})), QStringLiteral("text"));
        QCOMPARE(guessType(field({nullptr})), QStringLiteral("text"));
    }

    void buildsStatements()
    {
        std::vector<CsvRecord> records(2);
        records[0].fields = field({"a\tb", "1", "x"});
        records[1].fields = field({nullptr, "2"}); // Short: the rest are NULL.
        QCOMPARE(copyRows(records, {1, 0, 2}), QByteArray("1\ta\\tb\tx\n2\t\\N\t\\N\n"));
        QCOMPARE(copyStatement(QStringLiteral("public"), QStringLiteral("My Table"),
                               {QStringLiteral("id"), QStringLiteral("Name")}),
                 QByteArray("COPY public.\"My Table\" (id, \"Name\") FROM STDIN"));

        ImportPlan plan;
        plan.schema = QStringLiteral("s");
        plan.table = QStringLiteral("t");
        plan.columns = {{0, QStringLiteral("id"), QStringLiteral("integer")},
                        {1, QStringLiteral("when"), QStringLiteral("date")}};
        QCOMPARE(createTableStatement(plan),
                 QByteArray("CREATE TABLE s.t (\n    id integer,\n    \"when\" date\n)"));
    }
};

QTEST_MAIN(TestCsv)
#include "tst_csv.moc"
