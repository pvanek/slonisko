// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"
#include "ResultView.h"
#include "Shortcuts.h"

#include <QAction>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;

class TestShortcuts : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void keys()
    {
        const QList<QKeySequence> keys = Shortcuts::refresh();
#ifdef Q_OS_MACOS
        QCOMPARE(keys, QList<QKeySequence> {QKeySequence(Qt::CTRL | Qt::Key_R)}); // Cmd+R
#else
        QCOMPARE(
            keys,
            (QList<QKeySequence> {QKeySequence(Qt::Key_F5), QKeySequence(Qt::CTRL | Qt::Key_R)}));
#endif
    }

    void resultView()
    {
        ResultView view;
        int runs = 0;
        view.setRerun([&] { ++runs; });
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.table()->setFocus();
        for (const QKeySequence &key : Shortcuts::refresh()) {
            QTest::keySequence(view.table(), key);
        }
        QCOMPARE(runs, int(Shortcuts::refresh().size()));

        // Nothing to run again: nothing happens.
        view.setRerun({});
        QTest::keySequence(view.table(), Shortcuts::refresh().first());
        QCOMPARE(runs, int(Shortcuts::refresh().size()));
    }

    void connectionTree()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
        ConnectionBrowser browser(settings, false);
        QAction *refresh = nullptr;
        for (QAction *a : browser.actions()) {
            if (a->text().remove(QLatin1Char('&')) == QLatin1String("Refresh"))
                refresh = a;
        }
        QVERIFY(refresh);
        QCOMPARE(refresh->shortcuts(), Shortcuts::refresh());
        QCOMPARE(refresh->shortcutContext(), Qt::WidgetWithChildrenShortcut);
    }
};

QTEST_MAIN(TestShortcuts)
#include "tst_shortcuts.moc"
