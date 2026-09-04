// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>
#include <QSettings>

class QAction;
class QSplitter;
class QTabWidget;

namespace slonisko {

class ConnectionBrowser;
class EditorPage;
class FileBrowser;
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

    WorkspacePage *currentPage() const;
    // The current page if it is an editor.
    EditorPage *currentEditor() const;
    QList<WorkspacePage *> pages() const;

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
    // Closes a page, if it agrees (an editor may ask to save).
    bool closePage(int index);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenus();
    void updateTab(WorkspacePage *page);
    void updateActions();
    void openFile();
    void saveCurrent();
    void showAbout();

    QSettings m_settings;
    QSplitter *m_mainSplitter = nullptr;
    QTabWidget *m_left = nullptr; // Connections and files.
    ConnectionBrowser *m_browser = nullptr;
    FileBrowser *m_files = nullptr;
    QTabWidget *m_pages = nullptr; // The work area.
    QAction *m_save = nullptr;
    QAction *m_saveAs = nullptr;
    int m_editorCount = 0;
};

} // namespace slonisko
