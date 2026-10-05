// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include "ConnectionBrowser.h"
#include "EditorPage.h"
#include "FileBrowser.h"
#include "Icons.h"
#include "PageTabWidget.h"
#include "PageWindow.h"
#include "ResultModel.h"
#include "ObjectPage.h"
#include "ResultPage.h"
#include "HelpWindow.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>

namespace slonisko {

namespace {
const QString ProjectUrl = QStringLiteral("https://github.com/pvanek/slonisko");
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_mainSplitter(new QSplitter(Qt::Horizontal, this)),
      m_left(new QTabWidget(m_mainSplitter)),
      m_browser(new ConnectionBrowser(m_settings, true, m_left)),
      m_files(new FileBrowser(m_settings, m_left)), m_pages(new PageTabWidget(m_mainSplitter))
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

    m_pages->setMain(true);
    watchTabs(m_pages);
    connect(m_pages, &QTabWidget::currentChanged, this, [this] {
        if (EditorPage *editor = qobject_cast<EditorPage *>(m_pages->currentWidget()))
            editor->editor()->setFocus();
    });
    // Save and the like follow the active window.
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::updateActions);

    connect(m_browser, &ConnectionBrowser::editorRequested, this,
            [this](Session *s, const QString &database) { newEditor(s, database); });
    connect(m_browser, &ConnectionBrowser::monitoringRequested, this, &MainWindow::showResult);
    connect(m_browser, &ConnectionBrowser::objectRequested, this, &MainWindow::showObject);

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
    // Close the pages first: while the tab widgets delete them they would
    // report the remaining, half-destroyed ones as current.
    disconnect(qApp, nullptr, this, nullptr);
    for (PageWindow *window : std::as_const(m_windows)) {
        window->tabs()->disconnect(this);
        delete window;
    }
    m_pages->disconnect(this);
    while (m_pages->count() > 0)
        delete m_pages->widget(0);
}

void MainWindow::watchTabs(PageTabWidget *tabs)
{
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) {
        closePage(qobject_cast<WorkspacePage *>(tabs->widget(index)));
    });
    connect(tabs, &QTabWidget::currentChanged, this, &MainWindow::updateActions);
    connect(tabs, &PageTabWidget::detachRequested, this,
            [this](WorkspacePage *page) { detachPage(page); });
    connect(tabs, &PageTabWidget::moveToMainRequested, this,
            [this](WorkspacePage *page) { movePage(page, m_pages); });
}

WorkspacePage *MainWindow::currentPage() const
{
    // A page window, if one is active; else this window.
    for (PageWindow *window : m_windows) {
        if (window->isActiveWindow())
            return window->tabs()->currentPage();
    }
    return m_pages->currentPage();
}

EditorPage *MainWindow::currentEditor() const
{
    return qobject_cast<EditorPage *>(currentPage());
}

QList<WorkspacePage *> MainWindow::pages() const
{
    QList<WorkspacePage *> out = m_pages->pages();
    for (const PageWindow *window : m_windows)
        out += window->tabs()->pages();
    return out;
}

QList<PageWindow *> MainWindow::pageWindows() const
{
    return m_windows;
}

PageTabWidget *MainWindow::tabsOf(const WorkspacePage *page) const
{
    if (m_pages->indexOf(page) >= 0)
        return m_pages;
    for (PageWindow *window : m_windows) {
        if (window->tabs()->indexOf(page) >= 0)
            return window->tabs();
    }
    return nullptr;
}

void MainWindow::addPage(WorkspacePage *page)
{
    m_pages->addPage(page);
    connect(page, &WorkspacePage::titleChanged, this, [this, page] { updateTab(page); });
}

void MainWindow::updateTab(WorkspacePage *page)
{
    if (PageTabWidget *tabs = tabsOf(page))
        tabs->updatePage(page);
    for (PageWindow *window : std::as_const(m_windows)) {
        if (window->tabs()->currentPage() == page)
            window->setWindowTitle(page->title());
    }
}

PageWindow *MainWindow::detachPage(WorkspacePage *page)
{
    PageTabWidget *from = tabsOf(page);
    if (!from)
        return nullptr;
    // A window of its own, independent of this one: its own taskbar entry,
    // free to go behind it or to another screen. This window deletes it.
    auto *window = new PageWindow;
    window->setWindowIcon(windowIcon());
    m_windows << window;
    watchTabs(window->tabs());
    // Emptied: the window goes, later, as it may be handling a click of its own.
    connect(window->tabs(), &PageTabWidget::emptied, window, &QObject::deleteLater);
    connect(window, &QObject::destroyed, this, [this, window] {
        m_windows.removeAll(window);
        updateActions();
    });

    from->takePage(page);
    window->tabs()->addPage(page);
    window->setWindowTitle(page->title());
    // Smaller than this window and offset from its corner, cascading, so
    // this one stays in sight behind it.
    const QRect main = frameGeometry();
    window->resize(QSize(main.width() * 3 / 5, main.height() * 3 / 5).expandedTo(QSize(500, 350)));
    const int step = 40 * int(m_windows.size());
    window->move(main.topLeft() + QPoint(80 + step, 80 + step));
    window->show();
    return window;
}

void MainWindow::movePage(WorkspacePage *page, PageTabWidget *to)
{
    PageTabWidget *from = tabsOf(page);
    if (!from || from == to)
        return;
    from->takePage(page);
    to->addPage(page);
    to->window()->raise();
    to->window()->activateWindow();
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
    // Ctrl+click on a name in the script opens what it is.
    connect(page, &EditorPage::objectRequested, this,
            [this](Session *from, const QString &db, catalog::ObjectKind kind, unsigned int oid) {
                showObject(from, db, kind, oid, QString());
            });
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
            showPage(open);
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

ObjectPage *MainWindow::showObject(Session *session, const QString &database,
                                   catalog::ObjectKind kind, unsigned int oid, const QString &name)
{
    for (WorkspacePage *page : pages()) {
        auto *object = qobject_cast<ObjectPage *>(page);
        if (object && object->session() == session && object->oid() == oid
            && object->kind() == kind) {
            showPage(object);
            object->refresh();
            return object;
        }
    }
    auto *page = new ObjectPage(session, database, kind, oid, name);
    // A neighbour in the diagram opens its own page; its name comes from
    // the details it loads for itself.
    connect(page, &ObjectPage::objectRequested, this,
            [this](Session *from, const QString &db, catalog::ObjectKind neighbourKind,
                   unsigned int neighbour) {
                showObject(from, db, neighbourKind, neighbour, QString());
            });
    addPage(page);
    return page;
}

ResultPage *MainWindow::showResult(Session *session, const QString &title, const QByteArray &sql)
{
    for (WorkspacePage *page : pages()) {
        auto *result = qobject_cast<ResultPage *>(page);
        if (result && result->session() == session && result->sql() == sql) {
            showPage(result);
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
    return closePage(qobject_cast<WorkspacePage *>(m_pages->widget(index)));
}

bool MainWindow::closePage(WorkspacePage *page)
{
    PageTabWidget *tabs = page ? tabsOf(page) : nullptr;
    if (!tabs || !page->maybeClose())
        return false;
    tabs->takePage(page);
    delete page;
    // This window always has something to work in; page windows just go.
    if (m_pages->count() == 0)
        newEditor();
    return true;
}

void MainWindow::showPage(WorkspacePage *page)
{
    if (PageTabWidget *tabs = tabsOf(page)) {
        tabs->setCurrentWidget(page);
        tabs->window()->raise();
        tabs->window()->activateWindow();
    }
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
        showPage(page);
        if (!page->maybeClose()) {
            event->ignore();
            return;
        }
    }
    m_settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings.setValue(QStringLiteral("window/mainSplitter"), m_mainSplitter->saveState());
    // The page windows go with this one; their pages agreed above.
    for (PageWindow *window : std::as_const(m_windows)) {
        window->tabs()->disconnect(this);
        window->hide();
        window->deleteLater();
    }
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
    QAction *close = file->addAction(tr("&Close Tab"), this, [this] { closePage(currentPage()); });
    close->setShortcut(QKeySequence::Close);
    // Page windows have no menus: these work there too.
    for (QAction *action : {open, m_save, m_saveAs, close})
        action->setShortcutContext(Qt::ApplicationShortcut);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *manual = help->addAction(QIcon::fromTheme(QStringLiteral("help-contents")),
                                      tr("&Manual"), this, &MainWindow::showHelp);
    manual->setShortcut(QKeySequence::HelpContents); // F1
    manual->setShortcutContext(Qt::ApplicationShortcut);
    help->addSeparator();
    help->addAction(tr("&About Slonisko"), this, &MainWindow::showAbout);
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::showHelp()
{
    // The manual as built into the help file; without one, the website,
    // which is the same manual.
    if (HelpWindow::show(helpKeyword()))
        return;
    QDesktopServices::openUrl(QUrl(ProjectUrl));
}

// What the window is showing decides which page the manual opens.
QString MainWindow::helpKeyword() const
{
    const WorkspacePage *page = currentPage();
    if (qobject_cast<const EditorPage *>(page))
        return QStringLiteral("editor");
    if (qobject_cast<const ObjectPage *>(page))
        return QStringLiteral("diagrams");
    if (qobject_cast<const ResultPage *>(page))
        return QStringLiteral("results");
    return {};
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Slonisko"),
                       tr("<h3>Slonisko %1</h3>"
                          "<p>A PostgreSQL client.</p>"
                          "<p><a href=\"%2\">%2</a></p>"
                          "<p>Licensed under the GNU General Public License v3 or later.</p>")
                           .arg(QApplication::applicationVersion(), ProjectUrl));
}

} // namespace slonisko
