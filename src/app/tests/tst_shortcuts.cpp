// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"
#include "EditorPage.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "SearchBar.h"
#include "Shortcuts.h"
#include "SqlEditor.h"

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

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

    // Ctrl+R is Kate's replace in the editor, and runs the query again in
    // its results below.
    void editorSearch()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
        ConnectionBrowser browser(settings, false);
        EditorPage page(&browser, QStringLiteral("Editor"));
        page.resize(800, 600);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));

        page.editor()->setFocus();
        QTest::keyClick(page.editor(), Qt::Key_R, Qt::ControlModifier);
        QVERIFY(page.searchBar()->isVisible());
        QCOMPARE(page.searchBar()->mode(), SearchBar::Mode::Power);
        page.searchBar()->closeBar();
        QTest::keyClick(page.editor(), Qt::Key_F, Qt::ControlModifier);
        QVERIFY(page.searchBar()->isVisible());
        QCOMPARE(page.searchBar()->mode(), SearchBar::Mode::Incremental);
        page.searchBar()->closeBar();

        int runs = 0;
        ResultView *results = page.resultPanel()->results();
        results->setRerun([&] { ++runs; });
        // No rows yet, so no table: the view itself takes the keys.
        results->setFocusPolicy(Qt::StrongFocus);
        results->setFocus();
        QVERIFY(results->hasFocus());
        QTest::keyClick(results, Qt::Key_R, Qt::ControlModifier);
        QCOMPARE(runs, 1);
        QVERIFY(!page.searchBar()->isVisible());

        // And in the editor's menu.
        std::unique_ptr<QMenu> menu(page.editor()->createContextMenu());
        QVERIFY(menu->actions().contains(page.searchBar()->findAction()));
        QVERIFY(menu->actions().contains(page.searchBar()->replaceAction()));
    }
};

QTEST_MAIN(TestShortcuts)
#include "tst_shortcuts.moc"
