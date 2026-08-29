// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>
#include <QSettings>

class QSplitter;
class QTabWidget;

namespace slonisko {

class ConnectionBrowser;
class ResultView;

// The connection browser on the left, the editor top right and results
// below it.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenus();
    void newEditor();
    void showAbout();

    QSettings m_settings;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_workSplitter = nullptr;
    ConnectionBrowser *m_browser = nullptr;
    QTabWidget *m_editors = nullptr;
    ResultView *m_results = nullptr;
};

} // namespace slonisko
