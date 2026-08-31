// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Keywords.h"
#include "sql/Lexer.h"

#include <QTest>

using namespace slonisko::sql;

namespace {

const char *kindName(TokenKind kind)
{
    switch (kind) {
    case TokenKind::Whitespace:
        return "ws";
    case TokenKind::Comment:
        return "comment";
    case TokenKind::Keyword:
        return "kw";
    case TokenKind::Identifier:
        return "id";
    case TokenKind::QuotedIdentifier:
        return "qid";
    case TokenKind::String:
        return "str";
    case TokenKind::DollarDelimiter:
        return "dollar";
    case TokenKind::DollarString:
        return "dstr";
    case TokenKind::Number:
        return "num";
    case TokenKind::Operator:
        return "op";
    case TokenKind::Punctuation:
        return "punct";
    case TokenKind::Parameter:
        return "param";
    case TokenKind::PsqlVariable:
        return "var";
    case TokenKind::PsqlCommand:
        return "psql";
    case TokenKind::Unknown:
        return "?";
    }
    return "";
}

// "kind:text" for each significant token; "+" marks tokens inside a body.
QByteArrayList describe(const QByteArray &text)
{
    QByteArrayList out;
    for (const Token &t : tokenize(text)) {
        if (t.kind == TokenKind::Whitespace)
            continue;
        out << QByteArray(t.inBody ? "+" : "") + kindName(t.kind) + ':'
                + text.mid(t.offset, t.length);
    }
    return out;
}

// Each byte's token kind, for comparing ways of cutting the text.
QByteArray kindsPerByte(const QByteArray &text, const std::vector<qsizetype> &cuts)
{
    QByteArray out(text.size(), ' ');
    LexState state;
    qsizetype from = 0;
    auto run = [&](qsizetype to) {
        for (const Token &t : tokenize(QByteArrayView(text).sliced(from, to - from), state))
            for (qsizetype i = 0; i < t.length; ++i)
                out[from + t.offset + i] = char('A' + int(t.kind) + (t.inBody ? 32 : 0));
        from = to;
    };
    for (qsizetype cut : cuts)
        run(cut);
    run(text.size());
    return out;
}

} // namespace

class TestLexer : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void tokens_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QByteArrayList>("expected");

        QTest::newRow("select") << QByteArray("SELECT a, b FROM t;")
                                << QByteArrayList {"kw:SELECT", "id:a", "punct:,", "id:b",
                                                   "kw:FROM",   "id:t", "punct:;"};
        QTest::newRow("unreserved keyword")
            << QByteArray("select name") << QByteArrayList {"kw:select", "kw:name"};
        QTest::newRow("comments") << QByteArray("a -- x\n/* b /* c */ d */ e")
                                  << QByteArrayList {"id:a", "comment:-- x",
                                                     "comment:/* b /* c */ d */", "id:e"};
        QTest::newRow("strings") << QByteArray("'it''s' E'a\\'b' B'1010' X'1f' N'n' U&'d\\0061t'")
                                 << QByteArrayList {"str:'it''s'", "str:E'a\\'b'",
                                                    "str:B'1010'", "str:X'1f'",
                                                    "str:N'n'",    "str:U&'d\\0061t'"};
        QTest::newRow("backslash is literal in standard strings")
            << QByteArray("'C:\\' x") << QByteArrayList {"str:'C:\\'", "id:x"};
        QTest::newRow("quoted identifiers")
            << QByteArray("\"a\"\"b\" U&\"d\\0061t\"")
            << QByteArrayList {"qid:\"a\"\"b\"", "qid:U&\"d\\0061t\""};
        QTest::newRow("dollar quotes")
            << QByteArray("SELECT $x$ it's $$ fine $x$, $$a$$")
            << QByteArrayList {"kw:SELECT",  "dollar:$x$", "dstr: it's $$ fine ",
                               "dollar:$x$", "punct:,",    "dollar:$$",
                               "dstr:a",     "dollar:$$"};
        QTest::newRow("parameters and names with $")
            << QByteArray("$1 + a$b + $")
            << QByteArrayList {"param:$1", "op:+", "id:a$b", "op:+", "?:$"};
        QTest::newRow("numbers") << QByteArray("1 1.5 .5 1e10 1.5E-3 0x1F 0o17 0b101 1_000 1..10")
                                 << QByteArrayList {"num:1",    "num:1.5",    "num:.5",
                                                    "num:1e10", "num:1.5E-3", "num:0x1F",
                                                    "num:0o17", "num:0b101",  "num:1_000",
                                                    "num:1",    "punct:.",    "punct:.",
                                                    "num:10"};
        QTest::newRow("operators")
            << QByteArray("a->>'k' @> b <-> c ?| d #>> e != f")
            << QByteArrayList {"id:a",  "op:->>", "str:'k'", "op:@>", "id:b",  "op:<->", "id:c",
                               "op:?|", "id:d",   "op:#>>",  "id:e",  "op:!=", "id:f"};
        QTest::newRow("comment starts inside an operator")
            << QByteArray("a +-- x\n b */* y */ c")
            << QByteArrayList {"id:a", "op:+", "comment:-- x", "id:b", "op:*", "comment:/* y */",
                               "id:c"};
        QTest::newRow("casts and psql variables")
            << QByteArray("x::int := :v :'s' :\"i\" [1:2]")
            << QByteArrayList {"id:x",       "op:::",   "kw:int", "op::=",   "var::v", "var::'s'",
                               "var::\"i\"", "punct:[", "num:1",  "punct::", "num:2",  "punct:]"};
        QTest::newRow("psql command") << QByteArray("\\set x 1\nSELECT 1")
                                      << QByteArrayList {"psql:\\set x 1", "kw:SELECT", "num:1"};
        QTest::newRow("unicode identifiers")
            << QStringLiteral("SELECT příliš FROM žluťoučký").toUtf8()
            << QByteArrayList {"kw:SELECT", QStringLiteral("id:příliš").toUtf8(), "kw:FROM",
                               QStringLiteral("id:žluťoučký").toUtf8()};

        // Function and DO bodies are lexed as SQL.
        QTest::newRow("function body")
            << QByteArray("CREATE FUNCTION f() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql")
            << QByteArrayList {"kw:CREATE",   "kw:FUNCTION", "id:f",   "punct:(",
                               "punct:)",     "kw:RETURNS",  "kw:int", "kw:AS",
                               "dollar:$$",   "+kw:SELECT",  "+num:1", "dollar:$$",
                               "kw:LANGUAGE", "kw:sql"};
        QTest::newRow("DO body with nested dollar quote")
            << QByteArray("DO $f$ BEGIN EXECUTE $q$x$q$; END $f$")
            << QByteArrayList {"kw:DO",   "dollar:$f$", "+kw:BEGIN", "+kw:EXECUTE", "dollar:$q$",
                               "+dstr:x", "dollar:$q$", "+punct:;",  "+kw:END",     "dollar:$f$"};
        QTest::newRow("DO LANGUAGE body")
            << QByteArray("DO LANGUAGE plpgsql $$ BEGIN END $$")
            << QByteArrayList {"kw:DO",     "kw:LANGUAGE", "id:plpgsql", "dollar:$$",
                               "+kw:BEGIN", "+kw:END",     "dollar:$$"};
        QTest::newRow("body ends even inside a string, like in PostgreSQL")
            << QByteArray("AS $$ SELECT '$$ x")
            << QByteArrayList {"kw:AS", "dollar:$$", "+kw:SELECT", "+str:'", "dollar:$$", "id:x"};
        QTest::newRow("a comment between AS and the body")
            << QByteArray("AS /* c */ $$ x $$")
            << QByteArrayList {"kw:AS", "comment:/* c */", "dollar:$$", "+id:x", "dollar:$$"};
        QTest::newRow("dollar quote elsewhere is a string")
            << QByteArray("SELECT $$ SELECT 1 $$")
            << QByteArrayList {"kw:SELECT", "dollar:$$", "dstr: SELECT 1 ", "dollar:$$"};

        // Unterminated constructs run to the end.
        QTest::newRow("unterminated string")
            << QByteArray("SELECT 'abc") << QByteArrayList {"kw:SELECT", "str:'abc"};
        QTest::newRow("unterminated comment")
            << QByteArray("a /* /* */") << QByteArrayList {"id:a", "comment:/* /* */"};
        QTest::newRow("unterminated dollar")
            << QByteArray("SELECT $x$ abc")
            << QByteArrayList {"kw:SELECT", "dollar:$x$", "dstr: abc"};
    }

    void tokens()
    {
        QFETCH(QByteArray, text);
        QFETCH(QByteArrayList, expected);
        QCOMPARE(describe(text), expected);
    }

    void tokensCoverTheText()
    {
        const QByteArray text = "SELECT 'a' || $$b$$ -- c\n/* d */ FROM \"t\" WHERE x @> $1;";
        qsizetype expected = 0;
        for (const Token &t : tokenize(text)) {
            QCOMPARE(t.offset, expected);
            expected = t.end();
        }
        QCOMPARE(expected, text.size());
    }

    // Lexing line by line, as the editor does, must agree with lexing all
    // at once, for every way of cutting the text into lines.
    void resumable_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("multi-line constructs")
            << QByteArray("SELECT 'multi\nline' /* nested\n/* deep\n*/ still\n*/ \"q\nid\" "
                          "E'x\\\ny' $t$ one\ntwo $t$ x");
        QTest::newRow("function body")
            << QByteArray("CREATE FUNCTION f() RETURNS int\nLANGUAGE plpgsql AS\n$body$\n"
                          "DECLARE s text := 'a\nb';\nBEGIN\n  EXECUTE $q$SELECT\n1$q$;\n"
                          "  /* c\n */ RETURN 1;\nEND;\n$body$;\nSELECT 2;");
        QTest::newRow("DO LANGUAGE over lines")
            << QByteArray("DO\nLANGUAGE\nplpgsql\n$$ BEGIN\nEND\n$$;");
    }

    void resumable()
    {
        QFETCH(QByteArray, text);
        const QByteArray whole = kindsPerByte(text, {});

        std::vector<qsizetype> lines;
        for (qsizetype i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                lines.push_back(i + 1);
        QCOMPARE(kindsPerByte(text, lines), whole);

        // Also at any whitespace, where a token cannot be cut in two.
        for (qsizetype cut = 1; cut < text.size(); ++cut) {
            if (text[cut] != ' ' && text[cut - 1] != ' ' && text[cut - 1] != '\n')
                continue;
            const QByteArray split = kindsPerByte(text, {cut});
            if (split != whole)
                QFAIL(qPrintable(QStringLiteral("differs when cut at %1").arg(cut)));
        }
    }

    void stateAfterLine()
    {
        LexState state;
        tokenize("SELECT $body$ x", state);
        QCOMPARE(state.mode, LexState::Mode::DollarString);
        QCOMPARE(state.tag, QByteArray("$body$"));

        state = {};
        tokenize("/* a /* b */", state);
        QCOMPARE(state.mode, LexState::Mode::BlockComment);
        QCOMPARE(state.commentDepth, 1);

        state = {};
        tokenize("AS $f$ SELECT 'x", state);
        QCOMPARE(state.mode, LexState::Mode::String);
        QCOMPARE(state.bodyTag, QByteArray("$f$"));
    }

    void keywords()
    {
        QCOMPARE(keywordCategory("select"), KeywordCategory::Reserved);
        QCOMPARE(keywordCategory("SeLeCt"), KeywordCategory::Reserved);
        QCOMPARE(keywordCategory("name"), KeywordCategory::Unreserved);
        QCOMPARE(keywordCategory("int"), KeywordCategory::ColumnName);
        QCOMPARE(keywordCategory("left"), KeywordCategory::TypeFunctionName);
        QVERIFY(!keywordCategory("orders").has_value());
        QVERIFY(!keywordCategory("").has_value());
        QVERIFY(isBuiltinTypeName("jsonb"));
        QVERIFY(isBuiltinTypeName("TIMESTAMPTZ"));
        QVERIFY(!isBuiltinTypeName("orders"));
        // The list comes from the linked parser: sorted, lowercase, and
        // consistent with lookups. Guards the declarations in Keywords.cpp.
        const auto all = slonisko::sql::keywords();
        QVERIFY(all.size() > 400);
        QVERIFY(std::ranges::is_sorted(all));
        QCOMPARE(all.front(), QByteArray("abort"));
        for (const QByteArray &k : all)
            QVERIFY2(keywordCategory(k).has_value(), k.constData());
        QVERIFY(keywordCategory("between").has_value()); // Found by the parser's own hash.
    }

    void quoting()
    {
        QVERIFY(!needsQuoting("orders"));
        QVERIFY(!needsQuoting("order_items2"));
        QVERIFY(!needsQuoting("name")); // Unreserved keywords are fine.
        QVERIFY(needsQuoting("Orders"));
        QVERIFY(needsQuoting("order")); // Reserved.
        QVERIFY(needsQuoting("2fast"));
        QVERIFY(needsQuoting("with space"));
        QVERIFY(needsQuoting(QStringLiteral("příliš").toUtf8()));
    }
};

QTEST_GUILESS_MAIN(TestLexer)
#include "tst_lexer.moc"
