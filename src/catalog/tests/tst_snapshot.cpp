// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalog/Snapshot.h"

#include <QTest>

using namespace slonisko::catalog;

class TestSnapshot : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void searchPathResolution()
    {
        Snapshot s;
        s.searchPath = {QStringLiteral("app"), QStringLiteral("public")};
        s.relations = {
            {1, QStringLiteral("public"), QStringLiteral("orders"), 'r'},
            {2, QStringLiteral("app"), QStringLiteral("orders"), 'v'},
            {3, QStringLiteral("public"), QStringLiteral("customers"), 'r'},
        };

        QCOMPARE(s.findRelation({}, QStringLiteral("orders"))->oid, 2u);
        QCOMPARE(s.findRelation(QStringLiteral("public"), QStringLiteral("orders"))->oid, 1u);
        QCOMPARE(s.findRelation({}, QStringLiteral("customers"))->oid, 3u);
        QVERIFY(!s.findRelation({}, QStringLiteral("missing")));
        QVERIFY(!s.findRelation(QStringLiteral("other"), QStringLiteral("orders")));
    }
};

QTEST_GUILESS_MAIN(TestSnapshot)
#include "tst_snapshot.moc"
