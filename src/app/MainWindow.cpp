// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include "ConnectionBrowser.h"
#include "ResultView.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>

namespace slonisko {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_mainSplitter(new QSplitter(Qt::Horizontal, this)),
      m_workSplitter(new QSplitter(Qt::Vertical, m_mainSplitter)),
      m_browser(new ConnectionBrowser(m_settings, m_mainSplitter)),
      m_editors(new QTabWidget(m_workSplitter)), m_results(new ResultView(m_workSplitter))
{
    m_mainSplitter->addWidget(m_browser);
    m_mainSplitter->addWidget(m_workSplitter);
    m_mainSplitter->setStretchFactor(1, 1);
    m_workSplitter->addWidget(m_editors);
    m_workSplitter->addWidget(m_results);
    m_workSplitter->setStretchFactor(0, 3);
    m_workSplitter->setStretchFactor(1, 2);
    setCentralWidget(m_mainSplitter);

    m_editors->setDocumentMode(true);
    m_editors->setTabsClosable(true);
    m_editors->setMovable(true);
    connect(m_editors, &QTabWidget::tabCloseRequested, this, [this](int i) {
        delete m_editors->widget(i);
        if (m_editors->count() == 0)
            newEditor();
    });
    newEditor();

    connect(m_browser, &ConnectionBrowser::monitoringRequested, m_results, &ResultView::run);

    setupMenus();
    statusBar();

    resize(1280, 800);
    m_mainSplitter->setSizes({320, 960});
    restoreGeometry(m_settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_mainSplitter->restoreState(
        m_settings.value(QStringLiteral("window/mainSplitter")).toByteArray());
    m_workSplitter->restoreState(
        m_settings.value(QStringLiteral("window/workSplitter")).toByteArray());
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings.setValue(QStringLiteral("window/mainSplitter"), m_mainSplitter->saveState());
    m_settings.setValue(QStringLiteral("window/workSplitter"), m_workSplitter->saveState());
    event->accept();
}

void MainWindow::setupMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_browser->newConnectionAction());
    QAction *editor = file->addAction(tr("New &Editor"), this, &MainWindow::newEditor);
    editor->setShortcut(QKeySequence::New);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Slonisko"), this, &MainWindow::showAbout);
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::newEditor()
{
    // A placeholder until the real SQL editor exists.
    auto *editor = new QPlainTextEdit;
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setPlaceholderText(tr("SQL editor: not implemented yet."));
    const int i = m_editors->addTab(editor, tr("Script %1").arg(m_editors->count() + 1));
    m_editors->setCurrentIndex(i);
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
