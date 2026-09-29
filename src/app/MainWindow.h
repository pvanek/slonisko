// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QMainWindow>
#include <QSettings>

class QAction;
class QSplitter;
class QTabWidget;

namespace slonisko {

class ConnectionBrowser;
class EditorPage;
class FileBrowser;
class PageTabWidget;
class PageWindow;
class ObjectPage;
class ResultPage;
class Session;
class WorkspacePage;

// Connections and files on the left; the work area on the right, with a tab
// per page: SQL editors with their results, results on their own, and so on.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    ConnectionBrowser *browser() const { return m_browser; }
    FileBrowser *files() const { return m_files; }

    // The current page of the active window: this one or a page window.
    WorkspacePage *currentPage() const;
    // The current page if it is an editor.
    EditorPage *currentEditor() const;
    // All pages, in this window and in page windows.
    QList<WorkspacePage *> pages() const;
    QList<PageWindow *> pageWindows() const;
    // Where a page is shown, or null.
    PageTabWidget *tabsOf(const WorkspacePage *page) const;

    // Moves a page to a new window of its own, next to this one.
    PageWindow *detachPage(WorkspacePage *page);
    // Moves a page into other tabs, e.g. back into this window's.
    void movePage(WorkspacePage *page, PageTabWidget *to);
    PageTabWidget *mainTabs() const { return m_pages; }
    // Brings a page to the front, in whichever window it is.
    void showPage(WorkspacePage *page);

    // Adds a page to the work area and shows it; the window owns it then.
    void addPage(WorkspacePage *page);
    // A new editor on a session's database; without one, on the connection
    // selected in the browser, if any.
    EditorPage *newEditor(Session *session = nullptr, const QString &database = {});
    // Opens a file in the current editor if it is blank, else in a new one.
    EditorPage *openFile(const QString &path);
    // Shows a query's rows on a page of their own. The same query on the same
    // session reuses its page and runs again.
    ResultPage *showResult(Session *session, const QString &title, const QByteArray &sql);
    // A page with everything about one object; the same object reuses its page.
    ObjectPage *showObject(Session *session, const QString &database, catalog::ObjectKind kind,
                           unsigned int oid, const QString &name);
    // Closes a page, if it agrees (an editor may ask to save).
    bool closePage(int index);
    bool closePage(WorkspacePage *page);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenus();
    void watchTabs(PageTabWidget *tabs);
    void updateTab(WorkspacePage *page);
    void updateActions();
    void openFile();
    void saveCurrent();
    void showHelp();
    QString helpKeyword() const;
    void showAbout();

    QSettings m_settings;
    QSplitter *m_mainSplitter = nullptr;
    QTabWidget *m_left = nullptr; // Connections and files.
    ConnectionBrowser *m_browser = nullptr;
    FileBrowser *m_files = nullptr;
    PageTabWidget *m_pages = nullptr; // The work area.
    QList<PageWindow *> m_windows; // Pages dragged out of it.
    QAction *m_save = nullptr;
    QAction *m_saveAs = nullptr;
    int m_editorCount = 0;
};

} // namespace slonisko
