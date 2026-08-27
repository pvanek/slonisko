// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Splitter.h"

#include <QTest>

using slonisko::sql::splitStatements;

class TestSplitter : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void split_data()
    {
        QTest::addColumn<QByteArray>("script");
        QTest::addColumn<QByteArrayList>("expected");

        QTest::newRow("simple") << QByteArray("SELECT 1; SELECT 2;")
                                << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("no trailing semicolon")
            << QByteArray("SELECT 1;\nSELECT 2") << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("dollar quoted body")
            << QByteArray("DO $$ BEGIN PERFORM 1; END $$; SELECT 2;")
            << QByteArrayList {"DO $$", "SELECT 2"};
        QTest::newRow("nested comment") << QByteArray("SELECT 1 /* a /* b; */ c; */; SELECT 2;")
                                        << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("string with semicolon")
            << QByteArray("SELECT 'a;b'; SELECT 2;") << QByteArrayList {"SELECT 'a;b'", "SELECT 2"};
        QTest::newRow("does not need to parse") << QByteArray("SELEC oops FROM; SELECT 2;")
                                                << QByteArrayList {"SELEC oops", "SELECT 2"};
    }

    void split()
    {
        QFETCH(QByteArray, script);
        QFETCH(QByteArrayList, expected);

        const auto result = splitStatements(script);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.statements.size(), static_cast<std::size_t>(expected.size()));

        for (qsizetype i = 0; i < expected.size(); ++i) {
            const auto &span = result.statements[static_cast<std::size_t>(i)];
            const QByteArray text = script.mid(span.offset, span.length).trimmed();
            QVERIFY2(text.startsWith(expected[i]), text.constData());
        }
    }
};

QTEST_GUILESS_MAIN(TestSplitter)
#include "tst_splitter.moc"
