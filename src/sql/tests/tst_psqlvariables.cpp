// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/PsqlVariables.h"

#include <QTest>

using slonisko::sql::PsqlVariables;

class TestPsqlVariables : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void set()
    {
        PsqlVariables v;
        QVERIFY(v.apply("\\set limit 10"));
        QCOMPARE(v.value(QStringLiteral("limit")), QStringLiteral("10"));
        QVERIFY(v.apply("\\set empty"));
        QVERIFY(v.contains(QStringLiteral("empty")));
        QCOMPARE(v.value(QStringLiteral("empty")), QString());
        // Values are joined; single quotes go, with their escapes; double quotes stay.
        QVERIFY(v.apply("\\set msg 'it''s' ' a\\ttab' \"Id\""));
        QCOMPARE(v.value(QStringLiteral("msg")), QStringLiteral("it's a\ttab\"Id\""));
        // Variables inside arguments.
        QVERIFY(v.apply("\\set twice :limit:limit"));
        QCOMPARE(v.value(QStringLiteral("twice")), QStringLiteral("1010"));

        QVERIFY(v.apply("\\unset empty twice"));
        QVERIFY(!v.contains(QStringLiteral("empty")));
        QVERIFY(!v.contains(QStringLiteral("twice")));
    }

    void outputs()
    {
        PsqlVariables v;
        QString out;
        QVERIFY(v.apply("\\echo hello  'big world' :x", &out));
        QCOMPARE(out, QStringLiteral("hello big world :x"));
        v.set(QStringLiteral("a"), QStringLiteral("1"));
        v.set(QStringLiteral("b"), QStringLiteral("it's"));
        QVERIFY(v.apply("\\set", &out));
        QCOMPARE(out, QStringLiteral("a = '1'\nb = 'it''s'"));
        QVERIFY(!v.apply("\\connect db"));
        QVERIFY(!v.apply("SELECT 1"));
    }

    void substitute()
    {
        PsqlVariables v;
        v.set(QStringLiteral("n"), QStringLiteral("5"));
        v.set(QStringLiteral("name"), QStringLiteral("O'Brien"));
        v.set(QStringLiteral("path"), QStringLiteral("C:\\tmp"));
        v.set(QStringLiteral("table"), QStringLiteral("My Table"));

        QCOMPARE(v.substitute("SELECT :n, :'name', :\"table\""),
                 QByteArray("SELECT 5, 'O''Brien', \"My Table\""));
        QCOMPARE(v.substitute("SELECT :'path'"), QByteArray("SELECT  E'C:\\\\tmp'"));
        // Not in strings, comments, dollar quotes, casts or unset names.
        QCOMPARE(v.substitute("SELECT ':n', $$ :n $$, x::int, :missing -- :n"),
                 QByteArray("SELECT ':n', $$ :n $$, x::int, :missing -- :n"));
        QStringList unset;
        v.substitute("SELECT :missing, :'gone', :missing", nullptr, &unset);
        QCOMPARE(unset, (QStringList {QStringLiteral("missing"), QStringLiteral("gone")}));
        // Not in function bodies either.
        QCOMPARE(v.substitute("DO $$ BEGIN PERFORM :n; END $$"),
                 QByteArray("DO $$ BEGIN PERFORM :n; END $$"));
    }

    void offsets()
    {
        PsqlVariables v;
        v.set(QStringLiteral("v"), QStringLiteral("longer_value"));
        const QByteArray sql = "SELECT :v + nope";
        std::vector<PsqlVariables::Replacement> map;
        const QByteArray out = v.substitute(sql, &map);
        QCOMPARE(out, QByteArray("SELECT longer_value + nope"));
        // An error at "nope" in what was sent is at "nope" in the editor.
        QCOMPARE(PsqlVariables::originalOffset(out.indexOf("nope"), map), sql.indexOf("nope"));
        // Before the variable nothing moves; inside it, point at :v.
        QCOMPARE(PsqlVariables::originalOffset(3, map), 3);
        QCOMPARE(PsqlVariables::originalOffset(out.indexOf("value"), map), sql.indexOf(":v"));
    }
};

QTEST_GUILESS_MAIN(TestPsqlVariables)
#include "tst_psqlvariables.moc"
