// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/ValueFormat.h"

#include <QByteArrayList>
#include <QTest>

using namespace slonisko::catalog;

namespace {

// The tokens as "kind:text" pairs, to compare in one go.
QByteArrayList tokens(ValueSyntax syntax, QByteArrayView text)
{
    static const char *names[] = {"p", "key", "str", "num", "lit", "err"};
    QByteArrayList out;
    for (const ValueToken &t : tokenize(syntax, text))
        out << QByteArray(names[t.kind]) + ':' + text.sliced(t.start, t.length).toByteArray();
    return out;
}

// Formatted, or a marker that compares unequal to anything formatted.
QByteArray pj(QByteArrayView text)
{
    return prettyJson(text).value_or("<not JSON>");
}

QByteArray pa(QByteArrayView text)
{
    return prettyArray(text).value_or("<not an array>");
}

constexpr Oid Text = 25;
constexpr Oid Int4 = 23;
constexpr Oid Json = 114;
constexpr Oid Jsonb = 3802;
constexpr Oid Int4Array = 1007;
constexpr Oid SomeEnumArray = 16400; // A type the database made.

} // namespace

class TestValueFormat : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void guessesFromTheTypeFirst()
    {
        QCOMPARE(guessSyntax(Json, "not json"), ValueSyntax::Json);
        QCOMPARE(guessSyntax(Jsonb, "{}"), ValueSyntax::Json);
        QCOMPARE(guessSyntax(Int4Array, "{1,2}"), ValueSyntax::Array);
        QCOMPARE(guessSyntax(Int4, "42"), ValueSyntax::Text);
    }

    void guessesFromTheValueOtherwise()
    {
        QCOMPARE(guessSyntax(Text, "  {\"a\": [1, 2]} "), ValueSyntax::Json);
        QCOMPARE(guessSyntax(Text, "[true, null]"), ValueSyntax::Json);
        // Text is text, even when it looks like an array.
        QCOMPARE(guessSyntax(Text, "{a,b}"), ValueSyntax::Text);
        QCOMPARE(guessSyntax(Text, "{1,2}"), ValueSyntax::Text);
        QCOMPARE(guessSyntax(Text, "plain words"), ValueSyntax::Text);
        QCOMPARE(guessSyntax(Text, ""), ValueSyntax::Text);
        // An array of the database's own enum has an OID nobody knows.
        QCOMPARE(guessSyntax(SomeEnumArray, "{red,\"dark blue\"}"), ValueSyntax::Array);
        QCOMPARE(guessSyntax(SomeEnumArray, "(1,2)"), ValueSyntax::Text);
    }

    void tokenizesJson()
    {
        QCOMPARE(
            tokens(ValueSyntax::Json, "{\"a\": [1, -2.5e3, \"x\\\"y\"], \"b\" :true, \"c\":null}"),
            QByteArrayList({"p:{", "key:\"a\"", "p::", "p:[", "num:1", "p:,", "num:-2.5e3", "p:,",
                            "str:\"x\\\"y\"", "p:]", "p:,", "key:\"b\"", "p::", "lit:true", "p:,",
                            "key:\"c\"", "p::", "lit:null", "p:}"}));
        // What is not JSON is marked, and the rest still tokenized.
        QCOMPARE(tokens(ValueSyntax::Json, "[01, True, \"open"),
                 QByteArrayList({"p:[", "err:01", "p:,", "err:True", "p:,", "err:\"open"}));
    }

    void validatesJson()
    {
        QVERIFY(isValidJson("{\"a\": {\"b\": [1, {}, []]}}"));
        QVERIFY(isValidJson("\"just a string\""));
        QVERIFY(isValidJson(" 12 "));
        QVERIFY(!isValidJson("{1, 2}"));
        QVERIFY(!isValidJson("{\"a\" 1}"));
        QVERIFY(!isValidJson("[1, 2,]"));
        QVERIFY(!isValidJson("[1] [2]"));
        QVERIFY(!isValidJson("[1"));
        QVERIFY(!isValidJson(""));
        // Deeper than the parser goes: shown as it is rather than parsed.
        QVERIFY(!isValidJson(QByteArray(5000, '[') + QByteArray(5000, ']')));
    }

    void prettyJsonKeepsOrderAndNumbers()
    {
        // json keeps the members in their order and numbers as written;
        // a round trip through a double would turn the big one into 1e+20.
        const auto formatted
            = prettyJson("{\"z\":1,\"a\":[100000000000000000001,{},[]],\"m\":{\"k\":\"ü\"}}");
        QVERIFY(formatted);
        QCOMPARE(*formatted,
                 QByteArray("{\n"
                            "    \"z\": 1,\n"
                            "    \"a\": [\n"
                            "        100000000000000000001,\n"
                            "        {},\n"
                            "        []\n"
                            "    ],\n"
                            "    \"m\": {\n"
                            "        \"k\": \"ü\"\n"
                            "    }\n"
                            "}"));
        QCOMPARE(pj("[]"), QByteArray("[]"));
        QCOMPARE(pj("\"a,b:{c}\""), QByteArray("\"a,b:{c}\""));
        QVERIFY(!prettyJson("{oops}"));
    }

    void tokenizesArrays()
    {
        QCOMPARE(tokens(ValueSyntax::Array, "[0:1]={\"a \\\"b\\\"\",NULL, c d ,-1.5}"),
                 QByteArrayList({"p:[0:1]=", "p:{", "str:\"a \\\"b\\\"\"", "p:,", "lit:NULL", "p:,",
                                 "str:c d", "p:,", "num:-1.5", "p:}"}));
    }

    void prettyArrays()
    {
        // One element a line...
        QCOMPARE(pa("{a,\"b c\",NULL}"), QByteArray("{\n    a,\n    \"b c\",\n    NULL\n}"));
        // ...but a matrix keeps its rows on lines of their own.
        QCOMPARE(pa("{{1,2},{3,4}}"), QByteArray("{\n    {1,2},\n    {3,4}\n}"));
        QCOMPARE(pa("{{{1},{2}},{{3},{4}}}"),
                 QByteArray("{\n    {\n        {1},\n        {2}\n    },\n    {\n        {3},\n"
                            "        {4}\n    }\n}"));
        QCOMPARE(pa("[2:3]={x,y}"), QByteArray("[2:3]={\n    x,\n    y\n}"));
        QCOMPARE(pa("{}"), QByteArray("{}"));
        QVERIFY(!prettyArray("{1,2"));
        QVERIFY(!prettyArray("{1}}"));
        QVERIFY(!prettyArray("{1},{2}"));
        QVERIFY(!prettyArray("{\"open}"));
        QVERIFY(!prettyArray("plain"));
    }

    void textHasNoTokens()
    {
        QVERIFY(tokens(ValueSyntax::Text, "{\"a\": 1}").isEmpty());
        QVERIFY(!pretty(ValueSyntax::Text, "{\"a\": 1}"));
    }
};

QTEST_GUILESS_MAIN(TestValueFormat)
#include "tst_valueformat.moc"
