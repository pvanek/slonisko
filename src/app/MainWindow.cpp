// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include <QApplication>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>

namespace slonisko {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_tabs(new QTabWidget(this))
{
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    setCentralWidget(m_tabs);

    setupMenus();
    statusBar()->showMessage(tr("Not connected"));
    resize(1200, 800);
}

void MainWindow::setupMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *quit = file->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Slonisko"), this, &MainWindow::showAbout);
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Slonisko"),
                       tr("<h3>Slonisko %1</h3>"
                          "<p>A PostgreSQL client.</p>"
                          "<p>Licensed under the GNU General Public License v3 or later.</p>")
                           .arg(QApplication::applicationVersion()));
}

} // namespace slonisko
