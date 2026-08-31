// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CompletionPopup.h"
#include "SqlEditor.h"
#include "SqlLexer.h"

#include <QTest>

using namespace slonisko;

namespace {

int styleAt(SqlEditor &e, qsizetype pos)
{
    e.SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0, -1);
    return int(e.SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, static_cast<unsigned long>(pos)));
}

qsizetype find(SqlEditor &e, const char *text, int nth = 0)
{
    const QByteArray all = e.utf8Text();
    qsizetype at = -1;
    for (int i = 0; i <= nth; ++i)
        at = all.indexOf(text, at + 1);
    return at;
}

} // namespace

class TestSqlEditor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void highlighting()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT value, 'x' -- c\nFROM t WHERE id = $1::int4;"));
        QCOMPARE(styleAt(e, find(e, "SELECT")), int(SqlLexer::Keyword));
        QCOMPARE(styleAt(e, find(e, "value")), int(SqlLexer::UnreservedKeyword));
        QCOMPARE(styleAt(e, find(e, "'x'")), int(SqlLexer::String));
        QCOMPARE(styleAt(e, find(e, "-- c")), int(SqlLexer::Comment));
        QCOMPARE(styleAt(e, find(e, " t ") + 1), int(SqlLexer::Identifier));
        QCOMPARE(styleAt(e, find(e, "$1")), int(SqlLexer::Parameter));
        QCOMPARE(styleAt(e, find(e, "int4")), int(SqlLexer::Type));
        QCOMPARE(styleAt(e, find(e, "=")), int(SqlLexer::Operator));

        // After a dot it is a name, whatever else the word is.
        e.setText(QStringLiteral("SELECT c.name, c. text FROM c"));
        QCOMPARE(styleAt(e, find(e, "name")), int(SqlLexer::Identifier));
        QCOMPARE(styleAt(e, find(e, "text")), int(SqlLexer::Identifier));
    }

    // Editing one line restyles the following ones when it changes their
    // starting state, and only then.
    void restylesFollowingLines()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT 1;\nSELECT 2;\nSELECT 3;"));
        QCOMPARE(styleAt(e, find(e, "SELECT", 2)), int(SqlLexer::Keyword));

        e.insertAt(QStringLiteral("/* "), 0, 0);
        QCOMPARE(styleAt(e, find(e, "SELECT", 2)), int(SqlLexer::Comment));

        e.SendScintilla(QsciScintillaBase::SCI_DELETERANGE, 0UL, 3L);
        QCOMPARE(styleAt(e, find(e, "SELECT", 2)), int(SqlLexer::Keyword));
    }

    void functionBodyIsSql()
    {
        SqlEditor e;
        e.setText(
            QStringLiteral("CREATE FUNCTION f() RETURNS int AS $body$\nBEGIN\n  RETURN 'a;b';\n"
                           "END\n$body$ LANGUAGE plpgsql;\nSELECT $$not code$$;"));
        QCOMPARE(styleAt(e, find(e, "RETURN ")), int(SqlLexer::UnreservedKeyword));
        QCOMPARE(styleAt(e, find(e, "END\n")), int(SqlLexer::Keyword));
        QCOMPARE(styleAt(e, find(e, "'a;b'")), int(SqlLexer::String));
        QCOMPARE(styleAt(e, find(e, "$body$", 1)), int(SqlLexer::DollarDelimiter));
        QCOMPARE(styleAt(e, find(e, "not code")), int(SqlLexer::DollarString));
    }

    void folding()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT a,\n" // 0
                                 "       b\n" // 1
                                 "  FROM t;\n" // 2
                                 "SELECT 1;\n" // 3
                                 "CREATE FUNCTION f() RETURNS int AS $$\n" // 4
                                 "BEGIN\n" // 5
                                 "  IF x THEN\n" // 6
                                 "    RETURN 1;\n" // 7
                                 "  END IF;\n" // 8
                                 "  RETURN 2;\n" // 9
                                 "END\n" // 10
                                 "$$ LANGUAGE plpgsql;\n" // 11
                                 "/* multi\n" // 12
                                 "   line */\n" // 13
                                 "SELECT 2;")); // 14
        e.SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0, -1);
        auto level = [&](int line) {
            return int(e.SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL,
                                       static_cast<unsigned long>(line)));
        };
        auto depth = [&](int line) {
            return (level(line) & QsciScintillaBase::SC_FOLDLEVELNUMBERMASK)
                - QsciScintillaBase::SC_FOLDLEVELBASE;
        };
        auto header = [&](int line) {
            return (level(line) & QsciScintillaBase::SC_FOLDLEVELHEADERFLAG) != 0;
        };

        const QList<int> depths {0, 1, 1, 0, 0, 2, 3, 4, 4, 3, 3, 2, 0, 1, 0};
        for (int line = 0; line < depths.size(); ++line)
            QVERIFY2(depth(line) == depths[line],
                     qPrintable(QStringLiteral("line %1: %2").arg(line).arg(depth(line))));
        const QList<int> headers {0, 4, 5, 6, 12};
        for (int line = 0; line < depths.size(); ++line)
            QVERIFY2(header(line) == headers.contains(line), qPrintable(QString::number(line)));

        // A body in another language: its "if" does not leave the fold open.
        e.setText(QStringLiteral("CREATE FUNCTION g() RETURNS int LANGUAGE plpython3u AS $$\n"
                                 "if x:\n"
                                 "    return 1\n"
                                 "$$;\n"
                                 "SELECT 1;"));
        e.SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0, -1);
        QCOMPARE(depth(4), 0);
        QVERIFY(!header(4));

        e.setText(QStringLiteral("SELECT a,\n"
                                 "       b\n"
                                 "  FROM t;\n"
                                 "SELECT 1;\n"
                                 "CREATE FUNCTION f() RETURNS int AS $$\n"
                                 "BEGIN\n"
                                 "  IF x THEN\n"
                                 "    RETURN 1;\n"
                                 "  END IF;\n"
                                 "  RETURN 2;\n"
                                 "END\n"
                                 "$$ LANGUAGE plpgsql;\n"
                                 "/* multi\n"
                                 "   line */\n"
                                 "SELECT 2;"));
        e.SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0, -1);

        // Folding the function hides its body.
        e.SendScintilla(QsciScintillaBase::SCI_FOLDLINE, 4UL, 0L);
        QVERIFY(!e.SendScintilla(QsciScintillaBase::SCI_GETLINEVISIBLE, 7UL));
        QVERIFY(e.SendScintilla(QsciScintillaBase::SCI_GETLINEVISIBLE, 12UL));

        // An edit that only changes structure refolds the lines below it.
        e.SendScintilla(QsciScintillaBase::SCI_FOLDALL, 1UL); // Expand everything.
        e.insertAt(QStringLiteral("SELECT ("), 12, 0);
        e.SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0, -1);
        QVERIFY(depth(13) > 1);
        QVERIFY(depth(14) > 0);
    }

    void lineStateRoundTrip()
    {
        sql::LexState s;
        s.mode = sql::LexState::Mode::DollarString;
        s.tag = "$tag$";
        s.bodyTag = "$body$";
        s.commentDepth = 3;
        s.expect = sql::LexState::Expect::LanguageThenBody;
        QCOMPARE(SqlLexer::decode(SqlLexer::encode(s)), s);
        QVERIFY(SqlLexer::encode(s) >= 0);
        QCOMPARE(SqlLexer::decode(SqlLexer::encode({})), sql::LexState {});
    }

    void currentStatement()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT 1;\n\nSELECT 'ž',\n  2;\n\\set x 1\n"));
        e.setCursorPosition(find(e, "2;"));
        auto pieces = e.piecesToRun();
        QCOMPARE(pieces.size(), 1u);
        QCOMPARE(pieces[0].sql, QStringLiteral("SELECT 'ž',\n  2").toUtf8());
        QCOMPARE(pieces[0].offset, find(e, "SELECT", 1));

        e.setCursorPosition(find(e, "\n\n") + 1); // The empty line.
        QVERIFY(e.piecesToRun().empty());

        e.setCursorPosition(find(e, "\\set"));
        pieces = e.piecesToRun();
        QCOMPARE(pieces.size(), 1u);
        QVERIFY(pieces[0].psqlCommand);
    }

    void selectionRunsItsStatements()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT 1; SELECT 2; SELECT 3;"));
        e.selectRange(find(e, "SELECT 2"), e.utf8Text().size());
        const auto pieces = e.piecesToRun();
        QCOMPARE(pieces.size(), 2u);
        QCOMPARE(pieces[0].sql, QByteArray("SELECT 2"));
        QCOMPARE(pieces[1].offset, find(e, "SELECT 3"));
    }

    void copyData()
    {
        SqlEditor e;
        e.setText(QStringLiteral("COPY t FROM stdin;\n1\n2\n\\.\n"));
        e.setCursorPosition(find(e, "2\n"));
        const auto pieces = e.piecesToRun();
        QCOMPARE(pieces.size(), 1u);
        QCOMPARE(pieces[0].sql, QByteArray("COPY t FROM stdin"));
        QCOMPARE(pieces[0].copyData.value_or(QByteArray()), QByteArray("1\n2\n"));
    }

    void statementBounds()
    {
        const QByteArray text = "SELECT 1;\nSELECT a FROM t WHERE \nSELECT 3;";
        // Typing an unterminated statement: it runs to the next one.
        auto [start, end] = SqlEditor::statementBounds(text, text.indexOf("WHERE") + 6);
        QCOMPARE(text.sliced(start, end - start),
                 QByteArray("\nSELECT a FROM t WHERE \nSELECT 3;"));
        std::tie(start, end) = SqlEditor::statementBounds(text, 3);
        QCOMPARE(start, 0);
        std::tie(start, end) = SqlEditor::statementBounds(text, text.size());
        QCOMPARE(start, text.size());
    }

    void completionPopup()
    {
        auto snapshot = std::make_shared<catalog::Snapshot>();
        snapshot->searchPath = {QStringLiteral("public")};
        snapshot->schemas = {QStringLiteral("public")};
        snapshot->relations = {{1,
                                QStringLiteral("public"),
                                QStringLiteral("orders"),
                                'r',
                                {{QStringLiteral("customer_id"), QStringLiteral("integer")},
                                 {QStringLiteral("total"), QStringLiteral("numeric")}}}};

        SqlEditor e;
        e.setSnapshotProvider([snapshot] { return snapshot; });
        e.show();
        QVERIFY(QTest::qWaitForWindowExposed(&e));
        e.setFocus();
        e.setText(QStringLiteral(" FROM orders o"));
        e.setCursorPosition(0);
        QTest::keyClicks(&e, QStringLiteral("SELECT o."));
        CompletionPopup *popup = e.completionPopup();
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->count(), 2);
        QVERIFY(e.hasFocus()); // The popup does not take the focus.

        QTest::keyClick(&e, Qt::Key_Down);
        QCOMPARE(popup->current()->label, QStringLiteral("total"));
        QTest::keyClick(&e, Qt::Key_Return);
        QVERIFY(!popup->isVisible());
        QCOMPARE(e.text(), QStringLiteral("SELECT o.total FROM orders o"));

        // Typing filters; a single letter alone does not pop up the list.
        e.setText(QString());
        QTest::keyClicks(&e, QStringLiteral("SELECT * FROM o"));
        QTest::qWait(300);
        QVERIFY(!popup->isVisible());
        QTest::keyClicks(&e, QStringLiteral("r"));
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->current()->label, QStringLiteral("orders"));
        QTest::keyClick(&e, Qt::Key_Escape);
        QVERIFY(!popup->isVisible());

        // Ctrl+Space asks for it explicitly.
        e.setText(QStringLiteral("SELECT  FROM orders"));
        e.setCursorPosition(7);
        QTest::keyClick(&e, Qt::Key_Space, Qt::ControlModifier);
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->current()->label, QStringLiteral("customer_id"));
        QTest::keyClick(&e, Qt::Key_Tab);
        QCOMPARE(e.text(), QStringLiteral("SELECT customer_id FROM orders"));
    }

    void semanticHighlighting()
    {
        auto snapshot = std::make_shared<catalog::Snapshot>();
        snapshot->searchPath = {QStringLiteral("public")};
        snapshot->schemas = {QStringLiteral("public")};
        snapshot->relations = {{1,
                                QStringLiteral("public"),
                                QStringLiteral("orders"),
                                'r',
                                {{QStringLiteral("total"), QStringLiteral("numeric")}}}};

        SqlEditor e;
        e.setSnapshotProvider([snapshot] { return snapshot; });
        e.resize(600, 400);
        e.show();
        QVERIFY(QTest::qWaitForWindowExposed(&e));
        e.setText(QStringLiteral("SELECT o.total, o.nope FROM orders o JOIN ordrs x ON true;\n"
                                 "DO $$ x = 'py' # c $$ LANGUAGE plpython3u;"));
        e.refreshSemantics();
        QTRY_VERIFY(!e.semanticSpans().empty());

        using K = catalog::SemanticSpan::Kind;
        auto has = [&](const char *text, K kind) {
            const qsizetype pos = find(e, text);
            return e.SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT,
                                   static_cast<unsigned long>(SqlEditor::indicatorFor(kind)),
                                   static_cast<long>(pos))
                != 0;
        };
        QVERIFY(has("total", K::Column));
        QVERIFY(has("nope", K::UnknownColumn));
        QVERIFY(has("orders", K::Relation));
        QVERIFY(has("ordrs", K::UnknownRelation));
        QVERIFY(has("'py'", K::ForeignString));
        QVERIFY(has("# c", K::ForeignComment));
        QVERIFY(has("x = ", K::ForeignText));
        QVERIFY(!has("SELECT", K::Relation));

        QVERIFY(e.explanationAt(find(e, "total")).contains(QLatin1String("numeric")));
        QVERIFY(e.explanationAt(find(e, "ordrs")).contains(QLatin1String("Not in the catalog")));

        // Errors from the server explain themselves too.
        e.markError(find(e, "JOIN"), QStringLiteral("syntax error at or near JOIN"));
        QCOMPARE(e.explanationAt(find(e, "JOIN") + 1),
                 QStringLiteral("syntax error at or near JOIN"));
    }

    void errorMarks()
    {
        SqlEditor e;
        e.setText(QStringLiteral("SELECT nope FROM t;"));
        const qsizetype pos = find(e, "nope");
        e.markError(pos);
        QVERIFY(e.hasErrorAt(pos));
        QVERIFY(e.hasErrorAt(pos + 3));
        QVERIFY(!e.hasErrorAt(pos + 5));
        e.insert(QStringLiteral(" ")); // Any edit clears them.
        QVERIFY(!e.hasErrorAt(pos));
    }
};

QTEST_MAIN(TestSqlEditor)
#include "tst_sqleditor.moc"
