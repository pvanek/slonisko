// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Objects.h"
#include "catalog/Semantic.h"
#include "sql/EmbeddedLexer.h"

#include <QTest>

using namespace slonisko::catalog;
using Kind = SemanticSpan::Kind;

namespace {

Snapshot makeSnapshot()
{
    Snapshot s;
    s.searchPath = {QStringLiteral("pg_catalog"), QStringLiteral("public")};
    s.schemas = {QStringLiteral("pg_catalog"), QStringLiteral("public"), QStringLiteral("sales")};
    s.relations = {
        {1,
         QStringLiteral("public"),
         QStringLiteral("orders"),
         'r',
         {{QStringLiteral("id"), QStringLiteral("bigint")},
          {QStringLiteral("customer_id"), QStringLiteral("integer")},
          {QStringLiteral("total"), QStringLiteral("numeric")}}},
        {2,
         QStringLiteral("public"),
         QStringLiteral("customers"),
         'r',
         {{QStringLiteral("id"), QStringLiteral("integer")},
          {QStringLiteral("name"), QStringLiteral("text")}}},
        {3,
         QStringLiteral("sales"),
         QStringLiteral("invoices"),
         'v',
         {{QStringLiteral("amount"), QStringLiteral("numeric")}}},
    };
    s.functions = {{QStringLiteral("pg_catalog"), QStringLiteral("count"),
                    QStringLiteral("\"any\""), QStringLiteral("bigint"), 'a'},
                   {QStringLiteral("pg_catalog"), QStringLiteral("now"), QString(),
                    QStringLiteral("timestamp with time zone"), 'f'}};
    return s;
}

// "kind:text" for each span.
QByteArrayList describe(const QByteArray &script, bool withSnapshot = true)
{
    static const Snapshot snapshot = makeSnapshot();
    QByteArrayList out;
    for (const SemanticSpan &s : analyzeScript(script, withSnapshot ? &snapshot : nullptr)) {
        const char *kind = "";
        switch (s.kind) {
        case Kind::Relation:
            kind = "rel";
            break;
        case Kind::Cte:
            kind = "cte";
            break;
        case Kind::Column:
            kind = "col";
            break;
        case Kind::Function:
            kind = "fn";
            break;
        case Kind::UnknownRelation:
            kind = "?rel";
            break;
        case Kind::UnknownColumn:
            kind = "?col";
            break;
        case Kind::ForeignText:
            kind = "text";
            break;
        case Kind::ForeignKeyword:
            kind = "kw";
            break;
        case Kind::ForeignString:
            kind = "str";
            break;
        case Kind::ForeignComment:
            kind = "comment";
            break;
        case Kind::ForeignNumber:
            kind = "num";
            break;
        }
        out << QByteArray(kind) + ':' + script.mid(s.offset, s.length);
    }
    return out;
}

} // namespace

class TestSemantic : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void resolvesNames()
    {
        QCOMPARE(describe("SELECT o.total, name, count(*) FROM orders o JOIN customers c ON c.id = "
                          "o.customer_id"),
                 (QByteArrayList {"col:total", "col:name", "fn:count", "rel:orders",
                                  "rel:customers", "col:id", "col:customer_id"}));
    }

    // What Ctrl+click in the editor needs: the object a name turned out to be.
    void namesCarryTheirObject()
    {
        static const Snapshot snapshot = makeSnapshot();
        const auto spans = analyzeScript("SELECT o.total FROM orders o, sales.invoices", &snapshot);
        const auto relation = [&](const char *name) {
            return std::ranges::find_if(spans, [&](const SemanticSpan &span) {
                return span.kind == SemanticSpan::Kind::Relation
                    && span.detail.contains(QLatin1String(name));
            });
        };
        QVERIFY(relation("orders") != spans.end());
        QCOMPARE(relation("orders")->oid, 1u);
        QCOMPARE(relation("orders")->relationKind, 'r');
        QCOMPARE(relationKind(relation("orders")->relationKind), ObjectKind::Table);

        QCOMPARE(relation("invoices")->oid, 3u);
        QCOMPARE(relationKind(relation("invoices")->relationKind), ObjectKind::View);

        // An alias or a column names no object of its own.
        for (const SemanticSpan &span : spans) {
            if (span.kind == SemanticSpan::Kind::Column)
                QCOMPARE(span.oid, 0u);
        }
    }

    void schemaQualified()
    {
        QCOMPARE(describe("SELECT i.amount FROM sales.invoices i"),
                 (QByteArrayList {"col:amount", "rel:invoices"}));
        QCOMPARE(describe("SELECT sales.invoices.amount FROM sales.invoices"),
                 (QByteArrayList {"col:amount", "rel:invoices"}));
    }

    void unknownRelation()
    {
        QCOMPARE(describe("SELECT * FROM ordrs"), QByteArrayList {"?rel:ordrs"});
        // Only where data is read or written: DDL may name things that do not exist yet.
        QCOMPARE(describe("CREATE TABLE ordrs (a int)"), QByteArrayList {});
        QCOMPARE(describe("CREATE VIEW v AS SELECT id FROM ordrs"), QByteArrayList {});
        QCOMPARE(describe("SELECT 1 INTO new_table"), QByteArrayList {});
    }

    void createdInTheScript()
    {
        QCOMPARE(describe("CREATE TEMP TABLE scratch (a int);\nSELECT a FROM scratch;\nSELECT * "
                          "FROM missing;"),
                 QByteArrayList {"?rel:missing"});
        QCOMPARE(describe("SELECT * INTO TEMP made FROM orders; SELECT * FROM made;"),
                 QByteArrayList {"rel:orders"});
    }

    void unknownColumn()
    {
        QCOMPARE(describe("SELECT o.totl FROM orders o"),
                 (QByteArrayList {"?col:totl", "rel:orders"}));
        // Unqualified names might be output aliases or parameters: never flagged.
        QCOMPARE(describe("SELECT totl FROM orders"), QByteArrayList {"rel:orders"});
        // An alias used for two tables: too ambiguous to judge.
        QCOMPARE(describe("SELECT (SELECT x.nope FROM orders x), (SELECT x.name FROM customers x)"),
                 (QByteArrayList {"rel:orders", "rel:customers"}));
    }

    void ctesAndSubqueries()
    {
        QCOMPARE(describe("WITH r AS (SELECT id AS rid FROM orders) SELECT r.rid FROM r"),
                 (QByteArrayList {"col:id", "rel:orders", "col:rid", "cte:r"}));
        QCOMPARE(describe("SELECT s.x FROM (SELECT 1 AS x) s"), QByteArrayList {"col:x"});
    }

    void psqlVariablesStillParse()
    {
        QCOMPARE(
            describe("SELECT o.totl, count(*) FROM orders o JOIN custmers x ON true WHERE o.total "
                     "> :limit"),
            (QByteArrayList {"?col:totl", "fn:count", "rel:orders", "?rel:custmers", "col:total"}));
    }

    void brokenStatement()
    {
        // Does not parse: relations and alias.column from tokens, no unknown marks.
        QCOMPARE(describe("SELECT o.total, o.nope FROM orders o WHERE"),
                 (QByteArrayList {"col:total", "rel:orders"}));
    }

    void withoutSnapshot() { QCOMPARE(describe("SELECT * FROM orders", false), QByteArrayList {}); }

    void offsetsInScript()
    {
        static const Snapshot snapshot = makeSnapshot();
        const QByteArray script
            = QStringLiteral("SELECT 'žluť';\nSELECT total FROM orders;").toUtf8();
        const auto spans = analyzeScript(script, &snapshot);
        QCOMPARE(spans.size(), 2u);
        QCOMPARE(script.mid(spans[0].offset, spans[0].length), QByteArray("total"));
        QVERIFY(spans[0].detail.contains(QLatin1String("numeric")));

        // Only the statements in range.
        const auto first = analyzeScript(script, &snapshot, 0, 10);
        QVERIFY(first.empty());
    }

    void pythonBody()
    {
        const QByteArray sql = "CREATE FUNCTION f(n int) RETURNS int AS $$\n"
                               "# add one\n"
                               "if n is None: return 'none'\n"
                               "return n + 1\n"
                               "$$ LANGUAGE plpython3u";
        const QByteArrayList spans = describe(sql);
        QVERIFY(spans.first().startsWith("text:\n# add one"));
        QVERIFY(spans.contains(QByteArray("comment:# add one")));
        QVERIFY(spans.contains(QByteArray("kw:is")));
        QVERIFY(spans.contains(QByteArray("kw:None")));
        QVERIFY(spans.contains(QByteArray("str:'none'")));
        QVERIFY(spans.contains(QByteArray("num:1")));
    }

    void otherLanguages()
    {
        QVERIFY(describe("DO $$ var x = 'a'; // c\n $$ LANGUAGE plv8")
                    .contains(QByteArray("comment:// c")));
        QVERIFY(describe("DO LANGUAGE plperl $$ my $x = 1; $$").contains(QByteArray("kw:my")));
        QVERIFY(describe("CREATE FUNCTION f() RETURNS text AS 'return \"x\"' LANGUAGE plpython3u")
                    .contains(QByteArray("str:\"x\"")));
        // PL/pgSQL and SQL bodies stay SQL.
        QCOMPARE(describe("DO $$ BEGIN PERFORM 1; END $$"), QByteArrayList {});
        QCOMPARE(describe("CREATE FUNCTION f() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql"),
                 QByteArrayList {});
        // C functions have a symbol name, not code.
        QCOMPARE(describe("CREATE FUNCTION f() RETURNS int AS 'lib', 'sym' LANGUAGE c"),
                 QByteArrayList {});
    }

    void embeddedLexer()
    {
        using slonisko::sql::EmbeddedLanguage;
        using slonisko::sql::embeddedLanguage;
        QCOMPARE(embeddedLanguage(QStringLiteral("plpython3u")), EmbeddedLanguage::Python);
        QCOMPARE(embeddedLanguage(QStringLiteral("PLPGSQL")), EmbeddedLanguage::Sql);
        QCOMPARE(embeddedLanguage(QStringLiteral("plv8")), EmbeddedLanguage::JavaScript);
        QCOMPARE(embeddedLanguage(QStringLiteral("c")), EmbeddedLanguage::Other);

        const QByteArray python = "s = '''multi\nline''' # c";
        const auto tokens = slonisko::sql::tokenizeEmbedded(python, EmbeddedLanguage::Python);
        QCOMPARE(tokens.size(), 2u);
        QCOMPARE(python.mid(tokens[0].offset, tokens[0].length), QByteArray("'''multi\nline'''"));
        QCOMPARE(python.mid(tokens[1].offset, tokens[1].length), QByteArray("# c"));

        const QByteArray lua = "--[[ block ]] local x = [[s]] -- c";
        const auto l = slonisko::sql::tokenizeEmbedded(lua, EmbeddedLanguage::Lua);
        QCOMPARE(l.size(), 4u);
        QCOMPARE(lua.mid(l[0].offset, l[0].length), QByteArray("--[[ block ]]"));
    }
};

QTEST_GUILESS_MAIN(TestSemantic)
#include "tst_semantic.moc"
