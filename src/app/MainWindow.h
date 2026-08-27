// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>

class QTabWidget;

namespace slonisko {

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void setupMenus();
    void showAbout();

    QTabWidget *m_tabs = nullptr;
};

} // namespace slonisko
