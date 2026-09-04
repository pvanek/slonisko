// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include "ConnectionBrowser.h"
#include "EditorPage.h"
#include "FileBrowser.h"
#include "Icons.h"
#include "ResultModel.h"
#include "ResultPage.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>

namespace slonisko {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_mainSplitter(new QSplitter(Qt::Horizontal, this)),
      m_left(new QTabWidget(m_mainSplitter)),
      m_browser(new ConnectionBrowser(m_settings, true, m_left)),
      m_files(new FileBrowser(m_settings, m_left)), m_pages(new QTabWidget(m_mainSplitter))
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
    m_mainSplitter->addWidget(m_pages);
    m_mainSplitter->setStretchFactor(1, 1);
    setCentralWidget(m_mainSplitter);

    m_pages->setDocumentMode(true);
    m_pages->setTabsClosable(true);
    m_pages->setMovable(true);
    connect(m_pages, &QTabWidget::tabCloseRequested, this, &MainWindow::closePage);
    connect(m_pages, &QTabWidget::currentChanged, this, [this] {
        updateActions();
        if (EditorPage *editor = currentEditor())
            editor->editor()->setFocus();
    });

    connect(m_browser, &ConnectionBrowser::editorRequested, this,
            [this](Session *s, const QString &database) { newEditor(s, database); });
    connect(m_browser, &ConnectionBrowser::monitoringRequested, this, &MainWindow::showResult);

    setupMenus();
    statusBar();
    newEditor();

    resize(1280, 800);
    m_mainSplitter->setSizes({320, 960});
    restoreGeometry(m_settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_mainSplitter->restoreState(
        m_settings.value(QStringLiteral("window/mainSplitter")).toByteArray());
}

MainWindow::~MainWindow()
{
    // Close the pages first: while the tab widget deletes them it would
    // report the remaining, half-destroyed ones as current.
    m_pages->disconnect(this);
    while (m_pages->count() > 0)
        delete m_pages->widget(0);
}

WorkspacePage *MainWindow::currentPage() const
{
    return qobject_cast<WorkspacePage *>(m_pages->currentWidget());
}

EditorPage *MainWindow::currentEditor() const
{
    return qobject_cast<EditorPage *>(m_pages->currentWidget());
}

QList<WorkspacePage *> MainWindow::pages() const
{
    QList<WorkspacePage *> out;
    for (int i = 0; i < m_pages->count(); ++i) {
        if (auto *page = qobject_cast<WorkspacePage *>(m_pages->widget(i)))
            out << page;
    }
    return out;
}

void MainWindow::addPage(WorkspacePage *page)
{
    m_pages->addTab(page, page->title());
    connect(page, &WorkspacePage::titleChanged, this, [this, page] { updateTab(page); });
    updateTab(page);
    m_pages->setCurrentWidget(page);
}

void MainWindow::updateTab(WorkspacePage *page)
{
    const int index = m_pages->indexOf(page);
    if (index < 0)
        return;
    m_pages->setTabText(index, page->title());
    m_pages->setTabToolTip(index, page->toolTip());
    const QColor color = page->color();
    m_pages->setTabIcon(index, color.isValid() ? Icons::connection(color.name(), true) : QIcon());
}

void MainWindow::updateActions()
{
    const bool editor = currentEditor() != nullptr;
    m_save->setEnabled(editor);
    m_saveAs->setEnabled(editor);
}

EditorPage *MainWindow::newEditor(Session *session, const QString &database)
{
    auto *page = new EditorPage(m_browser, tr("Script %1").arg(++m_editorCount));
    QString db = database;
    if (!session)
        session = m_browser->currentSession(&db);
    if (!session && !m_browser->connectedSessions().empty())
        session = m_browser->connectedSessions().front();
    if (session)
        page->setSession(session, db);
    addPage(page);
    page->editor()->setFocus();
    return page;
}

EditorPage *MainWindow::openFile(const QString &path)
{
    // Already open: just show it.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (WorkspacePage *page : pages()) {
        auto *open = qobject_cast<EditorPage *>(page);
        if (open && open->filePath() == absolute) {
            m_pages->setCurrentWidget(open);
            return open;
        }
    }
    EditorPage *editor = currentEditor();
    if (!editor || !editor->isBlank())
        editor = newEditor(editor ? editor->session() : nullptr,
                           editor ? editor->database() : QString());
    QString error;
    if (!editor->openFile(path, &error)) {
        QMessageBox::warning(this, tr("Open File"), tr("Could not open %1:\n%2").arg(path, error));
        return nullptr;
    }
    m_pages->setCurrentWidget(editor);
    return editor;
}

ResultPage *MainWindow::showResult(Session *session, const QString &title, const QByteArray &sql)
{
    for (WorkspacePage *page : pages()) {
        auto *result = qobject_cast<ResultPage *>(page);
        if (result && result->session() == session && result->sql() == sql) {
            m_pages->setCurrentWidget(result);
            result->refresh();
            return result;
        }
    }
    auto *page = new ResultPage(session, title, sql);
    addPage(page);
    return page;
}

bool MainWindow::closePage(int index)
{
    auto *page = qobject_cast<WorkspacePage *>(m_pages->widget(index));
    if (!page || !page->maybeClose())
        return false;
    m_pages->removeTab(index);
    delete page;
    if (m_pages->count() == 0)
        newEditor();
    return true;
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
    EditorPage *editor = currentEditor();
    if (!editor)
        return;
    // In the result grid, with edits pending, Save means those.
    ResultView *results = editor->resultPanel()->results();
    const QWidget *focus = QApplication::focusWidget();
    if (focus && results->isAncestorOf(focus) && results->model()->hasChanges()) {
        editor->saveChanges();
        return;
    }
    editor->save();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (WorkspacePage *page : pages()) {
        m_pages->setCurrentWidget(page);
        if (!page->maybeClose()) {
            event->ignore();
            return;
        }
    }
    m_settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings.setValue(QStringLiteral("window/mainSplitter"), m_mainSplitter->saveState());
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
    m_save = file->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("&Save"), this,
                             &MainWindow::saveCurrent);
    m_save->setShortcut(QKeySequence::Save);
    m_saveAs = file->addAction(QIcon::fromTheme(QStringLiteral("document-save-as")),
                               tr("Save &As…"), this, [this] {
                                   if (EditorPage *page = currentEditor())
                                       page->saveAs();
                               });
    m_saveAs->setShortcut(QKeySequence::SaveAs);
    QAction *close
        = file->addAction(tr("&Close Tab"), this, [this] { closePage(m_pages->currentIndex()); });
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
