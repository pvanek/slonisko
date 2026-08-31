// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Splitter.h"

#include <QTest>

using slonisko::sql::splitStatements;
using slonisko::sql::StatementSpan;

namespace {

QByteArrayList texts(const QByteArray &script, const std::vector<StatementSpan> &spans)
{
    QByteArrayList out;
    for (const auto &span : spans)
        out << script.mid(span.offset, span.length);
    return out;
}

// Like texts(), but prefixes each span with its kind.
QByteArrayList describe(const QByteArray &script, const std::vector<StatementSpan> &spans)
{
    QByteArrayList out;
    for (const auto &span : spans) {
        const char *kind = span.kind == StatementSpan::Kind::Sql ? "sql: "
            : span.kind == StatementSpan::Kind::PsqlCommand      ? "cmd: "
                                                                 : "data: ";
        out << kind + script.mid(span.offset, span.length);
    }
    return out;
}

} // namespace

class TestSplitter : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void split_data()
    {
        QTest::addColumn<QByteArray>("script");
        QTest::addColumn<QByteArrayList>("expected");

        // Basics.
        QTest::newRow("empty") << QByteArray() << QByteArrayList {};
        QTest::newRow("whitespace only") << QByteArray(" \n\t\r\n ") << QByteArrayList {};
        QTest::newRow("comments only")
            << QByteArray("-- one\n/* two */ -- three") << QByteArrayList {};
        QTest::newRow("empty statements")
            << QByteArray(";; ;\nSELECT 1;;;") << QByteArrayList {"SELECT 1"};
        QTest::newRow("simple") << QByteArray("SELECT 1; SELECT 2;")
                                << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("no trailing semicolon")
            << QByteArray("SELECT 1;\nSELECT 2") << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("no space after semicolon")
            << QByteArray("SELECT 1;SELECT 2;") << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("multi-line")
            << QByteArray("SELECT a,\n       b\n  FROM t\n WHERE x = 1;\n\nUPDATE t SET a = 1;\n")
            << QByteArrayList {"SELECT a,\n       b\n  FROM t\n WHERE x = 1", "UPDATE t SET a = 1"};
        QTest::newRow("CRLF") << QByteArray("SELECT 1;\r\nSELECT\r\n 2;\r\n")
                              << QByteArrayList {"SELECT 1", "SELECT\r\n 2"};

        // Comments.
        QTest::newRow("leading comments excluded")
            << QByteArray("-- first\nSELECT 1; /* second */ SELECT 2;")
            << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("trailing comment excluded")
            << QByteArray("SELECT 1 -- note\n;") << QByteArrayList {"SELECT 1"};
        QTest::newRow("trailing comment at end of script")
            << QByteArray("SELECT 1 -- note") << QByteArrayList {"SELECT 1"};
        QTest::newRow("inner comment kept") << QByteArray("SELECT 1 -- one\n     + 2;")
                                            << QByteArrayList {"SELECT 1 -- one\n     + 2"};
        QTest::newRow("semicolon in line comment")
            << QByteArray("SELECT 1 -- ;\n; SELECT 2;") << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("semicolon in block comment")
            << QByteArray("SELECT 1 /* ; */ + 1; SELECT 2;")
            << QByteArrayList {"SELECT 1 /* ; */ + 1", "SELECT 2"};
        QTest::newRow("nested block comment")
            << QByteArray("SELECT 1 /* a /* b; */ c; */ + 1; SELECT 2;")
            << QByteArrayList {"SELECT 1 /* a /* b; */ c; */ + 1", "SELECT 2"};
        QTest::newRow("quote in comment")
            << QByteArray("SELECT 1 /* it's */; SELECT 2; -- don't\nSELECT 3;")
            << QByteArrayList {"SELECT 1", "SELECT 2", "SELECT 3"};
        QTest::newRow("dollars in comment")
            << QByteArray("SELECT 1; -- $$\nSELECT 2;") << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("comment starting inside operator")
            << QByteArray("SELECT 1 +-- ;\n 1; SELECT 2;")
            << QByteArrayList {"SELECT 1 +-- ;\n 1", "SELECT 2"};

        // Strings and quoted identifiers.
        QTest::newRow("semicolon in string")
            << QByteArray("SELECT 'a;b'; SELECT 2;") << QByteArrayList {"SELECT 'a;b'", "SELECT 2"};
        QTest::newRow("doubled quote") << QByteArray("SELECT 'it''s;'; SELECT 2;")
                                       << QByteArrayList {"SELECT 'it''s;'", "SELECT 2"};
        QTest::newRow("backslash is literal in standard string")
            << QByteArray("SELECT 'C:\\'; SELECT 2;")
            << QByteArrayList {"SELECT 'C:\\'", "SELECT 2"};
        QTest::newRow("escape string") << QByteArray("SELECT E'it\\'s;'; SELECT 2;")
                                       << QByteArrayList {"SELECT E'it\\'s;'", "SELECT 2"};
        QTest::newRow("escape string with escaped backslash")
            << QByteArray("SELECT e'\\\\'; SELECT 2;")
            << QByteArrayList {"SELECT e'\\\\'", "SELECT 2"};
        QTest::newRow("word ending in e is not an escape prefix")
            << QByteArray("SELECT type'\\'; SELECT 2;")
            << QByteArrayList {"SELECT type'\\'", "SELECT 2"};
        QTest::newRow("unicode escape string")
            << QByteArray("SELECT U&'d\\0061t;'; SELECT 2;")
            << QByteArrayList {"SELECT U&'d\\0061t;'", "SELECT 2"};
        QTest::newRow("comment markers in string")
            << QByteArray("SELECT '--', '/*'; SELECT 2;")
            << QByteArrayList {"SELECT '--', '/*'", "SELECT 2"};
        QTest::newRow("quoted identifier") << QByteArray("SELECT 1 AS \"a;b\"; SELECT 2;")
                                           << QByteArrayList {"SELECT 1 AS \"a;b\"", "SELECT 2"};
        QTest::newRow("quoted identifier with doubled quote")
            << QByteArray("SELECT 1 AS \"a\"\";b\"; SELECT 2;")
            << QByteArrayList {"SELECT 1 AS \"a\"\";b\"", "SELECT 2"};

        // Dollar quoting.
        QTest::newRow("DO block") << QByteArray("DO $$ BEGIN PERFORM 1; END $$; SELECT 2;")
                                  << QByteArrayList {"DO $$ BEGIN PERFORM 1; END $$", "SELECT 2"};
        QTest::newRow("tagged dollar quote")
            << QByteArray("DO $body$ BEGIN RAISE NOTICE '$$;'; END $body$; SELECT 2;")
            << QByteArrayList {"DO $body$ BEGIN RAISE NOTICE '$$;'; END $body$", "SELECT 2"};
        QTest::newRow("nested dollar quotes")
            << QByteArray("DO $f$ BEGIN EXECUTE $q$SELECT 1;$q$; END $f$; SELECT 2;")
            << QByteArrayList {"DO $f$ BEGIN EXECUTE $q$SELECT 1;$q$; END $f$", "SELECT 2"};
        QTest::newRow("dollar tags are case sensitive")
            << QByteArray("SELECT $a$ ; $A$ ; $a$; SELECT 2;")
            << QByteArrayList {"SELECT $a$ ; $A$ ; $a$", "SELECT 2"};
        QTest::newRow("dollar quoted string value")
            << QByteArray("SELECT $$it's; fine$$; SELECT 2;")
            << QByteArrayList {"SELECT $$it's; fine$$", "SELECT 2"};
        QTest::newRow("positional parameter is not a dollar quote")
            << QByteArray("PREPARE p AS SELECT $1; SELECT 2;")
            << QByteArrayList {"PREPARE p AS SELECT $1", "SELECT 2"};
        QTest::newRow("dollar inside identifier is not a dollar quote")
            << QByteArray("SELECT a$b$c; SELECT 2;") << QByteArrayList {"SELECT a$b$c", "SELECT 2"};

        // Functions and procedures.
        QTest::newRow("plpgsql function")
            << QByteArray("CREATE FUNCTION f(n int) RETURNS int\n"
                          "LANGUAGE plpgsql AS $$\n"
                          "DECLARE\n"
                          "    r int := 0;\n"
                          "BEGIN\n"
                          "    FOR i IN 1..n LOOP\n"
                          "        IF i % 2 = 0 THEN\n"
                          "            r := r + i;\n"
                          "        END IF;\n"
                          "    END LOOP;\n"
                          "    RETURN r;\n"
                          "END;\n"
                          "$$;\n"
                          "SELECT f(10);\n")
            << QByteArrayList {"CREATE FUNCTION f(n int) RETURNS int\n"
                               "LANGUAGE plpgsql AS $$\n"
                               "DECLARE\n"
                               "    r int := 0;\n"
                               "BEGIN\n"
                               "    FOR i IN 1..n LOOP\n"
                               "        IF i % 2 = 0 THEN\n"
                               "            r := r + i;\n"
                               "        END IF;\n"
                               "    END LOOP;\n"
                               "    RETURN r;\n"
                               "END;\n"
                               "$$",
                               "SELECT f(10)"};
        QTest::newRow("function body in a string")
            << QByteArray("CREATE FUNCTION f() RETURNS int AS 'SELECT 1; SELECT 2' LANGUAGE sql; "
                          "SELECT 3;")
            << QByteArrayList {
                   "CREATE FUNCTION f() RETURNS int AS 'SELECT 1; SELECT 2' LANGUAGE sql",
                   "SELECT 3"};
        QTest::newRow("BEGIN ATOMIC function")
            << QByteArray("CREATE FUNCTION f() RETURNS int LANGUAGE sql\n"
                          "BEGIN ATOMIC\n"
                          "    SELECT 1;\n"
                          "    SELECT 2;\n"
                          "END;\n"
                          "SELECT 3;")
            << QByteArrayList {"CREATE FUNCTION f() RETURNS int LANGUAGE sql\n"
                               "BEGIN ATOMIC\n"
                               "    SELECT 1;\n"
                               "    SELECT 2;\n"
                               "END",
                               "SELECT 3"};
        QTest::newRow("BEGIN ATOMIC procedure, lower case")
            << QByteArray("create or replace procedure p() language sql begin atomic "
                          "insert into t values (1); insert into t values (2); end; call p();")
            << QByteArrayList {"create or replace procedure p() language sql begin atomic "
                               "insert into t values (1); insert into t values (2); end",
                               "call p()"};
        QTest::newRow("CASE inside BEGIN ATOMIC")
            << QByteArray("CREATE FUNCTION f(x bool) RETURNS int LANGUAGE sql BEGIN ATOMIC "
                          "SELECT CASE WHEN x THEN 1 ELSE 2 END; END; SELECT 3;")
            << QByteArrayList {"CREATE FUNCTION f(x bool) RETURNS int LANGUAGE sql BEGIN ATOMIC "
                               "SELECT CASE WHEN x THEN 1 ELSE 2 END; END",
                               "SELECT 3"};
        QTest::newRow("nested CASE inside BEGIN ATOMIC")
            << QByteArray("CREATE FUNCTION f(x int) RETURNS int LANGUAGE sql BEGIN ATOMIC "
                          "SELECT CASE WHEN x > 0 THEN CASE WHEN x > 9 THEN 2 ELSE 1 END "
                          "ELSE 0 END; END; SELECT 3;")
            << QByteArrayList {"CREATE FUNCTION f(x int) RETURNS int LANGUAGE sql BEGIN ATOMIC "
                               "SELECT CASE WHEN x > 0 THEN CASE WHEN x > 9 THEN 2 ELSE 1 END "
                               "ELSE 0 END; END",
                               "SELECT 3"};
        QTest::newRow("comment between BEGIN and ATOMIC")
            << QByteArray("CREATE FUNCTION f() RETURNS int LANGUAGE sql BEGIN /* c */ ATOMIC "
                          "SELECT 1; END; SELECT 2;")
            << QByteArrayList {"CREATE FUNCTION f() RETURNS int LANGUAGE sql BEGIN /* c */ ATOMIC "
                               "SELECT 1; END",
                               "SELECT 2"};
        QTest::newRow("RETURN function body")
            << QByteArray("CREATE FUNCTION f() RETURNS int LANGUAGE sql RETURN 1; SELECT 2;")
            << QByteArrayList {"CREATE FUNCTION f() RETURNS int LANGUAGE sql RETURN 1", "SELECT 2"};
        QTest::newRow("parameter named begin")
            << QByteArray("CREATE FUNCTION f(begin int) RETURNS int LANGUAGE sql RETURN begin; "
                          "SELECT 2;")
            << QByteArrayList {"CREATE FUNCTION f(begin int) RETURNS int LANGUAGE sql RETURN begin",
                               "SELECT 2"};

        // Other statements that must not be mistaken for blocks.
        QTest::newRow("transaction block")
            << QByteArray("BEGIN; SELECT 1; END; BEGIN ISOLATION LEVEL SERIALIZABLE; COMMIT;")
            << QByteArrayList {"BEGIN", "SELECT 1", "END", "BEGIN ISOLATION LEVEL SERIALIZABLE",
                               "COMMIT"};
        QTest::newRow("CASE outside a function")
            << QByteArray("SELECT CASE WHEN true THEN 1 END; SELECT 2;")
            << QByteArrayList {"SELECT CASE WHEN true THEN 1 END", "SELECT 2"};
        QTest::newRow("semicolons inside parentheses")
            << QByteArray("CREATE RULE r AS ON INSERT TO t DO ALSO "
                          "(INSERT INTO a VALUES (1); INSERT INTO b VALUES (2)); SELECT 2;")
            << QByteArrayList {"CREATE RULE r AS ON INSERT TO t DO ALSO "
                               "(INSERT INTO a VALUES (1); INSERT INTO b VALUES (2))",
                               "SELECT 2"};
        QTest::newRow("numbers") << QByteArray("SELECT 1.5e10, 0x1F, 1_000, .5; SELECT 2;")
                                 << QByteArrayList {"SELECT 1.5e10, 0x1F, 1_000, .5", "SELECT 2"};

        // Text being edited.
        QTest::newRow("stray closing parenthesis")
            << QByteArray("SELECT 1); SELECT 2; SELECT 3;")
            << QByteArrayList {"SELECT 1)", "SELECT 2", "SELECT 3"};
        QTest::newRow("unclosed parenthesis runs to the end")
            << QByteArray("SELECT 1; SELECT (2; SELECT 3;")
            << QByteArrayList {"SELECT 1", "SELECT (2; SELECT 3;"};
        QTest::newRow("unterminated string runs to the end")
            << QByteArray("SELECT 1; SELECT 'abc; SELECT 3;")
            << QByteArrayList {"SELECT 1", "SELECT 'abc; SELECT 3;"};
        QTest::newRow("unterminated quoted identifier runs to the end")
            << QByteArray("SELECT 1; SELECT \"abc; SELECT 3;")
            << QByteArrayList {"SELECT 1", "SELECT \"abc; SELECT 3;"};
        QTest::newRow("unterminated dollar quote runs to the end")
            << QByteArray("SELECT 1; DO $$ BEGIN; SELECT 3;")
            << QByteArrayList {"SELECT 1", "DO $$ BEGIN; SELECT 3;"};
        QTest::newRow("unterminated block comment hides the rest")
            << QByteArray("SELECT 1; SELECT 2 /* ; SELECT 3;")
            << QByteArrayList {"SELECT 1", "SELECT 2"};
        QTest::newRow("unterminated BEGIN ATOMIC runs to the end")
            << QByteArray("SELECT 1; CREATE FUNCTION f() RETURNS int LANGUAGE sql BEGIN ATOMIC "
                          "SELECT 1; SELECT 2;")
            << QByteArrayList {"SELECT 1",
                               "CREATE FUNCTION f() RETURNS int LANGUAGE sql BEGIN "
                               "ATOMIC SELECT 1; SELECT 2;"};
        QTest::newRow("backslash at end of escape string")
            << QByteArray("SELECT E'\\") << QByteArrayList {"SELECT E'\\"};
        QTest::newRow("lone dollar at end")
            << QByteArray("SELECT $") << QByteArrayList {"SELECT $"};
    }

    void split()
    {
        QFETCH(QByteArray, script);
        QFETCH(QByteArrayList, expected);

        QCOMPARE(texts(script, splitStatements(script)), expected);
    }

    void terminated()
    {
        const auto spans = splitStatements("SELECT 1; SELECT 2 -- no semicolon\n");
        QCOMPARE(spans.size(), 2u);
        QVERIFY(spans[0].terminated);
        QVERIFY(!spans[1].terminated);

        const auto open = splitStatements("SELECT 'abc;");
        QCOMPARE(open.size(), 1u);
        QVERIFY(!open[0].terminated);

        const auto block = splitStatements(
            "CREATE FUNCTION f() RETURNS int LANGUAGE sql BEGIN ATOMIC SELECT 1; END;");
        QCOMPARE(block.size(), 1u);
        QVERIFY(block[0].terminated);
    }

    void offsetsAreUtf8Bytes()
    {
        const QByteArray script
            = QStringLiteral("SELECT 'žluťoučký kůň'; SELECT 1 AS příliš;").toUtf8();
        const auto spans = splitStatements(script);
        QCOMPARE(spans.size(), 2u);
        QCOMPARE(spans[1].offset, script.indexOf("SELECT 1"));
        QCOMPARE(texts(script, spans),
                 (QByteArrayList {QStringLiteral("SELECT 'žluťoučký kůň'").toUtf8(),
                                  QStringLiteral("SELECT 1 AS příliš").toUtf8()}));
    }

    void nonAsciiDollarTag()
    {
        const QByteArray script
            = QStringLiteral("DO $tělo$ BEGIN NULL; END $tělo$; SELECT 2;").toUtf8();
        QCOMPARE(texts(script, splitStatements(script)),
                 (QByteArrayList {QStringLiteral("DO $tělo$ BEGIN NULL; END $tělo$").toUtf8(),
                                  "SELECT 2"}));
    }

    void psql_data()
    {
        QTest::addColumn<QByteArray>("script");
        QTest::addColumn<QByteArrayList>("expected");

        // Meta-commands.
        QTest::newRow("command on its own line")
            << QByteArray("\\set ON_ERROR_STOP on\nSELECT 1;")
            << QByteArrayList {"cmd: \\set ON_ERROR_STOP on", "sql: SELECT 1"};
        QTest::newRow("trailing whitespace excluded")
            << QByteArray("\\set a 1  \t\nSELECT 1;")
            << QByteArrayList {"cmd: \\set a 1", "sql: SELECT 1"};
        QTest::newRow("CRLF") << QByteArray("\\set a 1\r\nSELECT 1;\r\n")
                              << QByteArrayList {"cmd: \\set a 1", "sql: SELECT 1"};
        QTest::newRow("command without arguments")
            << QByteArray("\\x\nSELECT 1;") << QByteArrayList {"cmd: \\x", "sql: SELECT 1"};
        QTest::newRow("two commands on a line")
            << QByteArray("\\x \\pset border 2\nSELECT 1;")
            << QByteArrayList {"cmd: \\x", "cmd: \\pset border 2", "sql: SELECT 1"};
        QTest::newRow("separator continues with SQL")
            << QByteArray("\\echo 'a;b' \\\\ SELECT 1; SELECT 2;")
            << QByteArrayList {"cmd: \\echo 'a;b'", "sql: SELECT 1", "sql: SELECT 2"};
        QTest::newRow("backslash inside quoted argument")
            << QByteArray("\\echo 'a\\\\b \\x' \"c\\d\" `echo \\e`\nSELECT 1;")
            << QByteArrayList {"cmd: \\echo 'a\\\\b \\x' \"c\\d\" `echo \\e`", "sql: SELECT 1"};
        QTest::newRow("quoted argument does not span lines")
            << QByteArray("\\echo 'oops\nSELECT 1;")
            << QByteArrayList {"cmd: \\echo 'oops", "sql: SELECT 1"};
        QTest::newRow("whole-line argument keeps backslashes")
            << QByteArray("\\! ls \\\\ echo hi\nSELECT 1;")
            << QByteArrayList {"cmd: \\! ls \\\\ echo hi", "sql: SELECT 1"};
        QTest::newRow("name with plus")
            << QByteArray("\\sf+ f(int)\n\\dt+ public.*\nSELECT 1;")
            << QByteArrayList {"cmd: \\sf+ f(int)", "cmd: \\dt+ public.*", "sql: SELECT 1"};
        QTest::newRow("pg_dump header")
            << QByteArray("\\restrict Ab12\n\nSET statement_timeout = 0;\n"
                          "\\connect mydb\n\\unrestrict Ab12\n")
            << QByteArrayList {"cmd: \\restrict Ab12", "sql: SET statement_timeout = 0",
                               "cmd: \\connect mydb", "cmd: \\unrestrict Ab12"};
        QTest::newRow("conditionals")
            << QByteArray("\\if :flag\nSELECT 1;\n\\else\nSELECT 2;\n\\endif\n")
            << QByteArrayList {"cmd: \\if :flag", "sql: SELECT 1", "cmd: \\else", "sql: SELECT 2",
                               "cmd: \\endif"};
        QTest::newRow("lone backslash at end")
            << QByteArray("SELECT 1;\n\\") << QByteArrayList {"sql: SELECT 1", "cmd: \\"};

        // Meta-commands and the query buffer.
        QTest::newRow("\\g ends the statement")
            << QByteArray("SELECT 1 \\g\nSELECT 2\n\\gx /tmp/out\n")
            << QByteArrayList {"sql: SELECT 1", "cmd: \\g", "sql: SELECT 2", "cmd: \\gx /tmp/out"};
        QTest::newRow("\\gset") << QByteArray("SELECT 1 AS x \\gset\n\\echo :x\n")
                                << QByteArrayList {"sql: SELECT 1 AS x", "cmd: \\gset",
                                                   "cmd: \\echo :x"};
        QTest::newRow("\\bind then \\g")
            << QByteArray("SELECT $1 \\bind 42 \\g\n")
            << QByteArrayList {"sql: SELECT $1", "cmd: \\bind 42", "cmd: \\g"};
        QTest::newRow("other command cuts the statement short")
            << QByteArray("SELECT 1\n\\echo hi\n+ 1;")
            << QByteArrayList {"sql: SELECT 1", "cmd: \\echo hi", "sql: + 1"};
        QTest::newRow("backslash semicolon stays in the statement")
            << QByteArray("SELECT 1 \\; SELECT 2; SELECT 3;")
            << QByteArrayList {"sql: SELECT 1 \\; SELECT 2", "sql: SELECT 3"};

        // Backslashes that are not meta-commands.
        QTest::newRow("backslash in string")
            << QByteArray("SELECT 'C:\\x', E'\\\\y'; SELECT 2;")
            << QByteArrayList {"sql: SELECT 'C:\\x', E'\\\\y'", "sql: SELECT 2"};
        QTest::newRow("backslash in comments")
            << QByteArray("-- \\x\n/* \\y */ SELECT 1;") << QByteArrayList {"sql: SELECT 1"};
        QTest::newRow("backslash in dollar quote")
            << QByteArray("DO $$ BEGIN RAISE NOTICE '\\x'; END $$;")
            << QByteArrayList {"sql: DO $$ BEGIN RAISE NOTICE '\\x'; END $$"};

        // COPY FROM STDIN.
        QTest::newRow("COPY data")
            << QByteArray("COPY public.t (a, b) FROM stdin;\n1\tone\n2\ttwo\n\\.\nSELECT 1;\n")
            << QByteArrayList {"sql: COPY public.t (a, b) FROM stdin", "data: 1\tone\n2\ttwo\n",
                               "sql: SELECT 1"};
        QTest::newRow("COPY data looks like SQL and psql")
            << QByteArray(
                   "COPY t FROM STDIN;\na;b\t'x\t\\N\t$$\n-- c\n\\set x\nx\\.\n\\.\nSELECT 1;")
            << QByteArrayList {"sql: COPY t FROM STDIN",
                               "data: a;b\t'x\t\\N\t$$\n-- c\n\\set x\nx\\.\n", "sql: SELECT 1"};
        QTest::newRow("empty COPY data")
            << QByteArray("copy t from stdin;\n\\.\nSELECT 1;")
            << QByteArrayList {"sql: copy t from stdin", "data: ", "sql: SELECT 1"};
        QTest::newRow("COPY data with CRLF")
            << QByteArray("COPY t FROM stdin;\r\n1\r\n\\.\r\nSELECT 1;\r\n")
            << QByteArrayList {"sql: COPY t FROM stdin", "data: 1\r\n", "sql: SELECT 1"};
        QTest::newRow("COPY with options")
            << QByteArray("COPY t (a) FROM STDIN WITH (FORMAT csv, HEADER);\na\n\"x;y\"\n\\.\n")
            << QByteArrayList {"sql: COPY t (a) FROM STDIN WITH (FORMAT csv, HEADER)",
                               "data: a\n\"x;y\"\n"};
        QTest::newRow("comment after COPY is not data")
            << QByteArray("COPY t FROM stdin; -- load\n1\n\\.\n")
            << QByteArrayList {"sql: COPY t FROM stdin", "data: 1\n"};
        QTest::newRow("end marker at end of script")
            << QByteArray("COPY t FROM stdin;\n1\n\\.")
            << QByteArrayList {"sql: COPY t FROM stdin", "data: 1\n"};
        QTest::newRow("COPY ended by \\g")
            << QByteArray("COPY t FROM stdin \\g\n1\n\\.\nSELECT 1;")
            << QByteArrayList {"sql: COPY t FROM stdin", "cmd: \\g", "data: 1\n", "sql: SELECT 1"};
        QTest::newRow("COPY TO STDOUT has no data")
            << QByteArray("COPY t TO STDOUT;\nSELECT 1;")
            << QByteArrayList {"sql: COPY t TO STDOUT", "sql: SELECT 1"};
        QTest::newRow("COPY from a file has no data")
            << QByteArray("COPY t FROM '/tmp/stdin';\nSELECT 1;")
            << QByteArrayList {"sql: COPY t FROM '/tmp/stdin'", "sql: SELECT 1"};
        QTest::newRow("stdin inside a COPY query is not data")
            << QByteArray("COPY (SELECT a FROM stdin) TO STDOUT;\nSELECT 1;")
            << QByteArrayList {"sql: COPY (SELECT a FROM stdin) TO STDOUT", "sql: SELECT 1"};
        QTest::newRow("FROM STDIN outside COPY is not data")
            << QByteArray("SELECT * FROM stdin;\nSELECT 1;")
            << QByteArrayList {"sql: SELECT * FROM stdin", "sql: SELECT 1"};
        QTest::newRow("\\copy from stdin")
            << QByteArray("\\copy t (a, b) FROM stdin with csv\n1,2\n\\.\nSELECT 1;")
            << QByteArrayList {"cmd: \\copy t (a, b) FROM stdin with csv", "data: 1,2\n",
                               "sql: SELECT 1"};
        QTest::newRow("\\copy to a file has no data")
            << QByteArray("\\copy t to 'stdin.csv'\nSELECT 1;")
            << QByteArrayList {"cmd: \\copy t to 'stdin.csv'", "sql: SELECT 1"};
    }

    void psql()
    {
        QFETCH(QByteArray, script);
        QFETCH(QByteArrayList, expected);

        QCOMPARE(describe(script, splitStatements(script)), expected);
    }

    void terminatedAroundCommands()
    {
        const QByteArray script = "SELECT 1 \\g\nSELECT 2\n\\echo x\nCOPY t FROM stdin;\n1\n";
        const auto spans = splitStatements(script);
        QCOMPARE(describe(script, spans),
                 (QByteArrayList {"sql: SELECT 1", "cmd: \\g", "sql: SELECT 2", "cmd: \\echo x",
                                  "sql: COPY t FROM stdin", "data: 1\n"}));
        QVERIFY(spans[0].terminated);
        QVERIFY(spans[1].terminated);
        QVERIFY(!spans[2].terminated);
        QVERIFY(spans[4].terminated);
        QVERIFY(!spans[5].terminated); // No \. before the end.
    }

    // Plain-format pg_dump output.
    void pgDump()
    {
        const QByteArray script = R"sql(--
-- PostgreSQL database dump
--

\restrict 3sQx9vLtdW2yHz

SET statement_timeout = 0;
SET client_encoding = 'UTF8';
SELECT pg_catalog.set_config('search_path', '', false);

CREATE TABLE public.orders (
    id integer NOT NULL,
    note text
);

--
-- Data for Name: orders; Type: TABLE DATA; Schema: public; Owner: -
--

COPY public.orders (id, note) FROM stdin;
1	first; with semicolon
2	\N
3	it's -- not a comment
\.


ALTER TABLE ONLY public.orders
    ADD CONSTRAINT orders_pkey PRIMARY KEY (id);

\unrestrict 3sQx9vLtdW2yHz
)sql";

        QCOMPARE(
            describe(script, splitStatements(script)),
            (QByteArrayList {
                "cmd: \\restrict 3sQx9vLtdW2yHz",
                "sql: SET statement_timeout = 0",
                "sql: SET client_encoding = 'UTF8'",
                "sql: SELECT pg_catalog.set_config('search_path', '', false)",
                "sql: CREATE TABLE public.orders (\n    id integer NOT NULL,\n    note text\n)",
                "sql: COPY public.orders (id, note) FROM stdin",
                "data: 1\tfirst; with semicolon\n2\t\\N\n3\tit's -- not a comment\n",
                "sql: ALTER TABLE ONLY public.orders\n"
                "    ADD CONSTRAINT orders_pkey PRIMARY KEY (id)",
                "cmd: \\unrestrict 3sQx9vLtdW2yHz",
            }));
    }

    void currentStatement_data()
    {
        QTest::addColumn<QByteArray>("script");
        QTest::addColumn<int>("pos");
        QTest::addColumn<int>("expected");

        const QByteArray two = "SELECT 1;\n\nSELECT 2;  -- x\n";
        QTest::newRow("inside the first") << two << 3 << 0;
        QTest::newRow("at the start") << two << 0 << 0;
        QTest::newRow("on the semicolon") << two << 8 << 0;
        QTest::newRow("after the semicolon") << two << 9 << 0;
        QTest::newRow("empty line between") << two << 10 << -1;
        QTest::newRow("start of the second") << two << 11 << 1;
        QTest::newRow("in the trailing comment") << two << 25 << 1;
        QTest::newRow("empty script") << QByteArray() << 0 << -1;

        const QByteArray indented = "SELECT 1;\n    SELECT 2;";
        QTest::newRow("before an indented one") << indented << 11 << 1;

        const QByteArray sameLine = "SELECT 1; SELECT 2;";
        QTest::newRow("between on one line prefers the one before") << sameLine << 9 << 0;

        const QByteArray copy = "COPY t FROM stdin;\n1\n2\n\\.\nSELECT 1;";
        QTest::newRow("COPY data runs the COPY") << copy << 20 << 0;
        QTest::newRow("after the data") << copy << 30 << 2;
    }

    void currentStatement()
    {
        QFETCH(QByteArray, script);
        QFETCH(int, pos);
        QFETCH(int, expected);
        QCOMPARE(slonisko::sql::statementAt(script, splitStatements(script), pos), expected);
    }

    // A typical file with several kinds of statements.
    void script()
    {
        const QByteArray script = R"sql(-- Schema for the orders module.
BEGIN;

CREATE TABLE orders (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    note text DEFAULT 'n/a; none',
    total numeric(12, 2) NOT NULL
);

/* Keep totals non-negative. */
CREATE FUNCTION check_total() RETURNS trigger
LANGUAGE plpgsql AS $fn$
BEGIN
    IF NEW.total < 0 THEN
        RAISE EXCEPTION 'negative total: %', NEW.total;
    END IF;
    RETURN NEW;
END;
$fn$;

CREATE TRIGGER orders_check BEFORE INSERT OR UPDATE ON orders
    FOR EACH ROW EXECUTE FUNCTION check_total();

CREATE FUNCTION order_count() RETURNS bigint
LANGUAGE sql STABLE
BEGIN ATOMIC
    SELECT count(*) FROM orders;
END;

DO $$
BEGIN
    INSERT INTO orders (total) VALUES (10), (20);
END
$$;

INSERT INTO orders (note, total) VALUES ('it''s -- not a comment', 5);

COMMIT;
)sql";

        const auto statements = texts(script, splitStatements(script));
        const QByteArrayList starts {
            "BEGIN",
            "CREATE TABLE orders (",
            "CREATE FUNCTION check_total()",
            "CREATE TRIGGER orders_check",
            "CREATE FUNCTION order_count()",
            "DO $$",
            "INSERT INTO orders (note, total)",
            "COMMIT",
        };
        QCOMPARE(statements.size(), starts.size());
        for (qsizetype i = 0; i < starts.size(); ++i)
            QVERIFY2(statements[i].startsWith(starts[i]), statements[i].constData());

        QVERIFY(statements[2].endsWith("END;\n$fn$"));
        QVERIFY(statements[4].endsWith("SELECT count(*) FROM orders;\nEND"));
        QVERIFY(statements[6].endsWith("5)"));
    }
};

QTEST_GUILESS_MAIN(TestSplitter)
#include "tst_splitter.moc"
