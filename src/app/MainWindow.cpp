// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include "ConnectionBrowser.h"
#include "EditorTab.h"
#include "FileBrowser.h"
#include "Icons.h"
#include "ResultModel.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "SqlEditor.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>

namespace slonisko {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_mainSplitter(new QSplitter(Qt::Horizontal, this)),
      m_workSplitter(new QSplitter(Qt::Vertical, m_mainSplitter)),
      m_left(new QTabWidget(m_mainSplitter)),
      m_browser(new ConnectionBrowser(m_settings, true, m_left)),
      m_files(new FileBrowser(m_settings, m_left)), m_editors(new QTabWidget(m_workSplitter)),
      m_results(new QStackedWidget(m_workSplitter))
{
    m_left->setDocumentMode(true);
    m_left->setTabPosition(QTabWidget::West);
    m_left->addTab(m_browser, QIcon::fromTheme(QStringLiteral("network-server-database")),
                   tr("Connections"));
    m_left->addTab(m_files, QIcon::fromTheme(QStringLiteral("folder")), tr("Files"));
    m_left->setCurrentIndex(m_settings.value(QStringLiteral("window/leftTab"), 0).toInt());
    connect(m_left, &QTabWidget::currentChanged, this,
            [this](int i) { m_settings.setValue(QStringLiteral("window/leftTab"), i); });
    connect(m_files, &FileBrowser::fileActivated, this,
            [this](const QString &path) { openFile(path); });
    m_mainSplitter->addWidget(m_left);
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
    connect(m_editors, &QTabWidget::tabCloseRequested, this, &MainWindow::closeEditor);
    connect(m_editors, &QTabWidget::currentChanged, this, [this] {
        if (EditorTab *tab = currentEditor()) {
            m_results->setCurrentWidget(tab->resultPanel());
            tab->editor()->setFocus();
        }
    });

    connect(m_browser, &ConnectionBrowser::editorRequested, this,
            [this](Session *s, const QString &database) { newEditor(s, database); });
    connect(m_browser, &ConnectionBrowser::monitoringRequested, this,
            [this](pg::QueryRunner *runner, const QString &title, const QByteArray &sql) {
                EditorTab *tab = currentEditor();
                if (!tab)
                    tab = newEditor();
                tab->resultPanel()->results()->run(runner, title, sql);
                tab->resultPanel()->showResults();
            });

    setupMenus();
    statusBar();
    newEditor();

    resize(1280, 800);
    m_mainSplitter->setSizes({320, 960});
    restoreGeometry(m_settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_mainSplitter->restoreState(
        m_settings.value(QStringLiteral("window/mainSplitter")).toByteArray());
    m_workSplitter->restoreState(
        m_settings.value(QStringLiteral("window/workSplitter")).toByteArray());
}

MainWindow::~MainWindow()
{
    // Close the editors first: while the tab widget deletes its pages it
    // would report the remaining, half-destroyed ones as current.
    m_editors->disconnect(this);
    while (m_editors->count() > 0)
        delete m_editors->widget(0);
}

EditorTab *MainWindow::currentEditor() const
{
    return qobject_cast<EditorTab *>(m_editors->currentWidget());
}

EditorTab *MainWindow::newEditor(Session *session, const QString &database)
{
    auto *tab = new EditorTab(m_browser, tr("Script %1").arg(++m_editorCount));
    m_results->addWidget(tab->resultPanel());
    const int index = m_editors->addTab(tab, tab->title());
    connect(tab, &EditorTab::titleChanged, this, [this, tab] { updateTab(tab); });

    QString db = database;
    if (!session)
        session = m_browser->currentSession(&db);
    if (!session && !m_browser->connectedSessions().empty())
        session = m_browser->connectedSessions().front();
    if (session)
        tab->setSession(session, db);

    updateTab(tab);
    m_editors->setCurrentIndex(index);
    tab->editor()->setFocus();
    return tab;
}

void MainWindow::updateTab(EditorTab *tab)
{
    const int index = m_editors->indexOf(tab);
    if (index < 0)
        return;
    m_editors->setTabText(index, tab->title());
    m_editors->setTabToolTip(index, tab->filePath());
    const QColor color = tab->color();
    m_editors->setTabIcon(index, color.isValid() ? Icons::connection(color.name(), true) : QIcon());
}

EditorTab *MainWindow::openFile(const QString &path)
{
    // Already open: just show it.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_editors->count(); ++i) {
        auto *open = qobject_cast<EditorTab *>(m_editors->widget(i));
        if (open && open->filePath() == absolute) {
            m_editors->setCurrentIndex(i);
            return open;
        }
    }
    EditorTab *tab = currentEditor();
    if (!tab || !tab->isBlank())
        tab = newEditor(tab ? tab->session() : nullptr, tab ? tab->database() : QString());
    QString error;
    if (!tab->openFile(path, &error)) {
        QMessageBox::warning(this, tr("Open File"), tr("Could not open %1:\n%2").arg(path, error));
        return nullptr;
    }
    m_editors->setCurrentWidget(tab);
    return tab;
}

void MainWindow::openFile()
{
    const QString dir = m_settings.value(QStringLiteral("files/lastDirectory")).toString();
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Open SQL Script"), dir, tr("SQL scripts (*.sql *.psql);;All files (*)"));
    for (const QString &path : paths)
        openFile(path);
    if (!paths.isEmpty())
        m_settings.setValue(QStringLiteral("files/lastDirectory"),
                            QFileInfo(paths.first()).absolutePath());
}

void MainWindow::saveCurrent()
{
    EditorTab *tab = currentEditor();
    if (!tab)
        return;
    // In the result grid, with edits pending, Save means those.
    ResultView *results = tab->resultPanel()->results();
    const QWidget *focus = QApplication::focusWidget();
    if (focus && results->isAncestorOf(focus) && results->model()->hasChanges()) {
        tab->saveChanges();
        return;
    }
    tab->save();
}

void MainWindow::closeEditor(int index)
{
    auto *tab = qobject_cast<EditorTab *>(m_editors->widget(index));
    if (!tab || !tab->maybeSave())
        return;
    m_editors->removeTab(index);
    delete tab; // Also deletes its result panel.
    if (m_editors->count() == 0)
        newEditor();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < m_editors->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_editors->widget(i));
        if (tab && tab->isModified()) {
            m_editors->setCurrentIndex(i);
            if (!tab->maybeSave()) {
                event->ignore();
                return;
            }
        }
    }
    m_settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings.setValue(QStringLiteral("window/mainSplitter"), m_mainSplitter->saveState());
    m_settings.setValue(QStringLiteral("window/workSplitter"), m_workSplitter->saveState());
    event->accept();
}

void MainWindow::setupMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_browser->newConnectionAction());
    QAction *editor = file->addAction(QIcon::fromTheme(QStringLiteral("document-new")),
                                      tr("New SQL &Editor"), this, [this] { newEditor(); });
    editor->setShortcut(QKeySequence::New);
    QAction *open = file->addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                    tr("&Open File…"), this, qOverload<>(&MainWindow::openFile));
    open->setShortcut(QKeySequence::Open);
    QAction *save = file->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("&Save"),
                                    this, &MainWindow::saveCurrent);
    save->setShortcut(QKeySequence::Save);
    QAction *saveAs = file->addAction(QIcon::fromTheme(QStringLiteral("document-save-as")),
                                      tr("Save &As…"), this, [this] {
                                          if (EditorTab *tab = currentEditor())
                                              tab->saveAs();
                                      });
    saveAs->setShortcut(QKeySequence::SaveAs);
    QAction *close = file->addAction(tr("&Close Editor"), this,
                                     [this] { closeEditor(m_editors->currentIndex()); });
    close->setShortcut(QKeySequence::Close);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), this, &QWidget::close);
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
