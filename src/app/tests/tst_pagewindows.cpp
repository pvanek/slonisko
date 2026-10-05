// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"
#include "PageTabWidget.h"
#include "PageWindow.h"
#include "WorkspacePage.h"

#include <QAction>
#include <QApplication>
#include <QMessageBox>
#include <QPointer>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

using namespace slonisko;

namespace {

// The simplest page there can be.
class TestPage : public WorkspacePage
{
public:
    explicit TestPage(const QString &title) : m_title(title) { }
    QString title() const override { return m_title; }
    QColor color() const override { return Qt::red; }
    bool maybeClose() override { return closable; }

    void rename(const QString &title)
    {
        m_title = title;
        Q_EMIT titleChanged();
    }

    bool closable = true;

private:
    QString m_title;
};

} // namespace

class TestPageWindows : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void detachAndReturn()
    {
        MainWindow w;
        auto *page = new TestPage(QStringLiteral("Locks"));
        w.addPage(page);
        QCOMPARE(w.tabsOf(page), w.mainTabs());

        PageWindow *window = w.detachPage(page);
        QVERIFY(window);
        QCOMPARE(w.tabsOf(page), window->tabs());
        QCOMPARE(w.mainTabs()->indexOf(page), -1);
        QVERIFY(w.pages().contains(page)); // Still found, wherever it is.
        QCOMPARE(w.pageWindows().size(), 1);
        QCOMPARE(window->windowTitle(), QStringLiteral("Locks"));
        QVERIFY(!window->tabs()->isMain());

        // Titles follow the page into its window.
        page->rename(QStringLiteral("Locks now"));
        QCOMPARE(window->tabs()->tabText(0), QStringLiteral("Locks now"));
        QCOMPARE(window->windowTitle(), QStringLiteral("Locks now"));

        // Back: the emptied window goes away.
        QPointer<PageWindow> gone = window;
        w.movePage(page, w.mainTabs());
        QCOMPARE(w.tabsOf(page), w.mainTabs());
        QTRY_VERIFY(!gone);
        QVERIFY(w.pageWindows().isEmpty());
        QCOMPARE(w.mainTabs()->tabText(w.mainTabs()->indexOf(page)), QStringLiteral("Locks now"));
    }

    void moveBetweenWindows()
    {
        MainWindow w;
        auto *a = new TestPage(QStringLiteral("A"));
        auto *b = new TestPage(QStringLiteral("B"));
        w.addPage(a);
        w.addPage(b);
        PageWindow *window = w.detachPage(a);
        w.movePage(b, window->tabs());
        QCOMPARE(w.tabsOf(b), window->tabs());
        QCOMPARE(window->tabs()->count(), 2);
        // What "Move to Main Window" in the tab's context menu does.
        Q_EMIT window->tabs()->moveToMainRequested(a);
        QCOMPARE(w.tabsOf(a), w.mainTabs());
        QCOMPARE(window->tabs()->count(), 1);
        // And "Move to New Window".
        Q_EMIT w.mainTabs()->detachRequested(a);
        QCOMPARE(w.pageWindows().size(), 2);
    }

    // The new window is independent, and must not hide the main one: it is
    // smaller and sits offset from its corner.
    void newWindowKeepsMainInSight()
    {
        MainWindow w;
        w.resize(1000, 700);
        w.move(50, 50);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *page = new TestPage(QStringLiteral("Locks"));
        w.addPage(page);
        PageWindow *window = w.detachPage(page);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QVERIFY(w.isVisible());
        QVERIFY(!w.isMinimized());
        QCOMPARE(window->parentWidget(), nullptr); // Independent of the main window.
        QVERIFY(window->isWindow());
        const QRect main = w.frameGeometry();
        const QRect detached = window->frameGeometry();
        QVERIFY(detached.width() < main.width());
        QVERIFY(detached.height() < main.height());
        QVERIFY(detached.left() > main.left() && detached.top() > main.top());

        // A second one cascades.
        auto *other = new TestPage(QStringLiteral("Sessions"));
        w.addPage(other);
        PageWindow *second = w.detachPage(other);
        QVERIFY(second->pos() != window->pos());
    }

    void closingPages()
    {
        MainWindow w;
        auto *a = new TestPage(QStringLiteral("A"));
        auto *b = new TestPage(QStringLiteral("B"));
        w.addPage(a);
        w.addPage(b);
        PageWindow *window = w.detachPage(a);
        w.movePage(b, window->tabs());

        // Closing one tab keeps the window; closing the window closes the rest.
        QPointer<TestPage> first = a, second = b;
        QVERIFY(w.closePage(a));
        QVERIFY(!first);
        QPointer<PageWindow> gone = window;
        second->closable = false; // "Cancel" in its save question.
        QVERIFY(!window->close());
        QVERIFY(gone);
        second->closable = true;
        QVERIFY(window->close());
        QTRY_VERIFY(!gone);
        QTRY_VERIFY(!second);
    }

    void mainWindowTakesPageWindowsAlong()
    {
        auto w = std::make_unique<MainWindow>();
        w->show();
        auto *page = new TestPage(QStringLiteral("Sessions"));
        w->addPage(page);
        QPointer<PageWindow> window = w->detachPage(page);
        QVERIFY(w->close());
        QTRY_VERIFY(!window);
        w.reset();
    }

    void destroyedWithOpenWindows()
    {
        // Regression guard: deleting the main window with page windows open.
        auto w = std::make_unique<MainWindow>();
        auto *page = new TestPage(QStringLiteral("x"));
        w->addPage(page);
        w->detachPage(page);
        w.reset();
    }

    void aboutLinksToTheProject()
    {
        MainWindow w;
        QAction *about = nullptr;
        for (QAction *action : w.findChildren<QAction *>()) {
            if (action->text() == QLatin1String("&About Slonisko"))
                about = action;
        }
        QVERIFY(about);

        // The dialog is modal: read it from inside its own event loop.
        QString text;
        QTimer::singleShot(0, &w, [&text] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                text = box->text();
                box->accept();
            }
        });
        about->trigger();
        QVERIFY2(text.contains(QLatin1String("href=\"https://github.com/pvanek/slonisko\"")),
                 qPrintable(text));
    }
};

QTEST_MAIN(TestPageWindows)
#include "tst_pagewindows.moc"
