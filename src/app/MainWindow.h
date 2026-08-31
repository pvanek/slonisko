// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>
#include <QSettings>

class QSplitter;
class QStackedWidget;
class QTabWidget;

namespace slonisko {

class ConnectionBrowser;
class EditorTab;
class FileBrowser;
class Session;

// The connection browser on the left, SQL editors top right and the current
// editor's results below them.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    ConnectionBrowser *browser() const { return m_browser; }
    FileBrowser *files() const { return m_files; }
    EditorTab *currentEditor() const;
    // A new editor on a session's database; without one, on the connection
    // selected in the browser, if any.
    EditorTab *newEditor(Session *session = nullptr, const QString &database = {});
    // Opens a file in the current editor if it is blank, else in a new one.
    EditorTab *openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenus();
    void closeEditor(int index);
    void updateTab(EditorTab *tab);
    void openFile();
    void saveCurrent();
    void showAbout();

    QSettings m_settings;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_workSplitter = nullptr;
    QTabWidget *m_left = nullptr; // Connections and files.
    ConnectionBrowser *m_browser = nullptr;
    FileBrowser *m_files = nullptr;
    QTabWidget *m_editors = nullptr;
    QStackedWidget *m_results = nullptr;
    int m_editorCount = 0;
};

} // namespace slonisko
