// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ObjectPage.h"

#include "ErdView.h"
#include "Icons.h"
#include "ResultTextView.h"
#include "Session.h"
#include "Shortcuts.h"
#include "catalog/ErdExport.h"
#include "pg/QueryRunner.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTableView>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <memory>

namespace slonisko {

namespace {

// One of an object's lists: the rows as the catalog query returned them.
class DetailModel : public QAbstractTableModel
{
public:
    DetailModel(catalog::DetailTable table, QObject *parent)
        : QAbstractTableModel(parent), m_table(std::move(table))
    { }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(m_table.rows.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(m_table.columns.size());
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole))
            return {};
        return m_table.rows[std::size_t(index.row())].value(index.column());
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        return orientation == Qt::Horizontal ? m_table.columns.value(section)
                                             : QVariant(section + 1);
    }

private:
    catalog::DetailTable m_table;
};

QTableView *viewOf(const catalog::DetailTable &table, QWidget *parent)
{
    auto *view = new QTableView(parent);
    view->setModel(new DetailModel(table, view));
    view->setAlternatingRowColors(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 6);
    view->horizontalHeader()->setStretchLastSection(true);
    view->resizeColumnsToContents();
    return view;
}

} // namespace

ObjectPage::ObjectPage(Session *session, const QString &database, catalog::ObjectKind kind,
                       catalog::Oid oid, const QString &name, QWidget *parent)
    : WorkspacePage(parent), m_session(session), m_database(database), m_kind(kind), m_oid(oid),
      m_name(name.isEmpty() ? tr("Loading…") : name), m_heading(new QLabel(this)),
      m_message(new QLabel(this)), m_tabs(new QTabWidget(this)),
      m_definition(new ResultTextView(this))
{
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    m_heading->setMargin(4);
    m_heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setMargin(8);
    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_tabs->setDocumentMode(true);
    // They only ever appear as tabs; loose, they would sit over the heading.
    m_definition->hide();
    if (catalog::hasDiagram(m_kind)) {
        m_diagramTab = new QWidget(this);
        m_diagram = new ErdView(m_diagramTab);
        auto *diagramBar = new QToolBar(m_diagramTab);
        diagramBar->setIconSize(QSize(16, 16));
        auto diagramAction = [&](const QString &icon, const QString &text, auto slot) {
            QAction *action = diagramBar->addAction(QIcon::fromTheme(icon), text);
            connect(action, &QAction::triggered, m_diagram, slot);
            return action;
        };
        diagramAction(QStringLiteral("zoom-fit-best"), tr("Fit"), &ErdView::fitDiagram);
        diagramAction(QStringLiteral("zoom-in"), tr("Zoom In"), &ErdView::zoomIn);
        diagramAction(QStringLiteral("zoom-out"), tr("Zoom Out"), &ErdView::zoomOut);
        diagramAction(QStringLiteral("zoom-original"), tr("Actual Size"), &ErdView::resetZoom);
        diagramBar->addSeparator();
        // Text formats are mostly pasted into documentation, so they can go
        // straight to the clipboard; pictures only make sense as files.
        auto *exportMenu = new QMenu(diagramBar);
        exportMenu->setObjectName(QStringLiteral("diagramExportMenu"));
        auto copyAction = [&](const QString &objectName, const QString &text, auto toText) {
            QAction *action = exportMenu->addAction(text);
            action->setObjectName(objectName);
            connect(action, &QAction::triggered, this, [this, toText] {
                if (m_diagram && !m_diagram->graph().isEmpty())
                    QGuiApplication::clipboard()->setText(toText(m_diagram->graph()));
            });
        };
        copyAction(QStringLiteral("copyMermaid"), tr("Copy as Mermaid"), &catalog::erdToMermaid);
        copyAction(QStringLiteral("copyGraphviz"), tr("Copy as Graphviz"), &catalog::erdToDot);
        exportMenu->addSeparator();
        const QStringList filters = ErdView::fileFilters();
        const QStringList saveTexts = {tr("Save as SVG…"), tr("Save as PNG…"), tr("Save as PDF…"),
                                       tr("Save as Graphviz…"), tr("Save as Mermaid…")};
        for (int i = 0; i < filters.size(); ++i) {
            QAction *action = exportMenu->addAction(saveTexts.value(i, filters[i]));
            connect(action, &QAction::triggered, this,
                    [this, filter = filters[i]] { exportDiagram(filter); });
        }
        // Nothing to copy or save until the keys have been read.
        connect(exportMenu, &QMenu::aboutToShow, this, [this, exportMenu] {
            const bool ready = m_diagram && !m_diagram->graph().isEmpty();
            for (QAction *action : exportMenu->actions())
                action->setEnabled(ready);
        });
        auto *exportButton = new QToolButton(diagramBar);
        exportButton->setIcon(
            QIcon::fromTheme(QStringLiteral("document-export"),
                             QIcon::fromTheme(QStringLiteral("document-save-as"))));
        exportButton->setText(tr("Export"));
        exportButton->setToolTip(
            tr("Copy the diagram as Mermaid or Graphviz, or save it to a file"));
        exportButton->setMenu(exportMenu);
        exportButton->setPopupMode(QToolButton::InstantPopup);
        exportButton->setToolButtonStyle(diagramBar->toolButtonStyle());
        diagramBar->addWidget(exportButton);
        auto *hint = new QLabel(tr("Drag to move a table, Ctrl+wheel to zoom, "
                                   "double-click a neighbour to open it."),
                                m_diagramTab);
        hint->setEnabled(false);
        diagramBar->addWidget(hint);
        // Reading the keys is two more queries, so they wait until the tab
        // is actually opened.
        connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
            if (m_tabs->currentWidget() == m_diagramTab && !m_diagramLoaded)
                loadDiagram();
        });
        auto *diagramLayout = new QVBoxLayout(m_diagramTab);
        diagramLayout->setContentsMargins(0, 0, 0, 0);
        diagramLayout->setSpacing(0);
        diagramLayout->addWidget(diagramBar);
        diagramLayout->addWidget(m_diagram, 1);
        m_diagramTab->hide();
        connect(m_diagram, &ErdView::tableActivated, this, [this](unsigned int neighbour) {
            Q_EMIT objectRequested(m_session, m_database, catalog::ObjectKind::Table, neighbour);
        });
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // The heading, with the connection's actions beside it: what is waiting
    // here for an answer that never comes waits on that connection.
    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(m_heading, 1);
    auto *connectionBar = new QToolBar(this);
    connectionBar->setIconSize(QSize(16, 16));
    m_reconnect = connectionBar->addAction(Icons::reconnect(), tr("Reconnect"), this, [this] {
        if (m_session)
            Q_EMIT reconnectRequested(m_session);
    });
    m_reconnect->setToolTip(tr("Close the connection to this server and open it again, e.g. "
                               "when the network left it hanging"));
    m_disconnect = connectionBar->addAction(Icons::disconnect(), tr("Disconnect"), this, [this] {
        if (m_session)
            Q_EMIT disconnectRequested(m_session);
    });
    m_disconnect->setToolTip(tr("Disconnect from this server"));
    top->addWidget(connectionBar);
    layout->addLayout(top);
    layout->addWidget(m_message);
    layout->addWidget(m_tabs, 1);

    auto *refreshAction
        = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Refresh"), this);
    refreshAction->setShortcuts(Shortcuts::refresh());
    refreshAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(refreshAction, &QAction::triggered, this, &ObjectPage::refresh);
    addAction(refreshAction);

    if (m_session)
        connect(m_session, &Session::stateChanged, this, &ObjectPage::onSessionState);
    m_reconnect->setEnabled(m_session);
    m_disconnect->setEnabled(m_session);
    refresh();
}

void ObjectPage::onSessionState()
{
    const Session::State state = m_session ? m_session->state() : Session::State::Disconnected;
    // Gone for good once disconnected: the tree makes a new session to
    // connect again, and this page stays with the old one.
    m_reconnect->setEnabled(m_session && state != Session::State::Disconnected);
    m_disconnect->setEnabled(m_session && state != Session::State::Disconnected);
    switch (state) {
    case Session::State::Connected:
        refresh(); // Back after reconnecting.
        break;
    case Session::State::Connecting:
        // What was asked of the old connections now fails; that is no news.
        ++m_generation;
        showMessage(tr("Reconnecting…"));
        break;
    case Session::State::Failed:
        showMessage(tr("Could not reconnect: %1").arg(m_session->errorMessage()), true);
        break;
    case Session::State::Disconnected:
        showMessage(tr("Not connected."));
        break;
    }
}

QColor ObjectPage::color() const
{
    return m_session ? QColor::fromString(m_session->profile().color) : QColor();
}

void ObjectPage::refresh()
{
    pg::QueryRunner *runner = m_session ? m_session->runner(m_database) : nullptr;
    const std::vector<QByteArray> queries = catalog::detailQueries(m_kind, m_oid);
    if (!runner || queries.empty()) {
        showMessage(runner ? tr("Nothing is shown for this kind of object.")
                           : tr("Not connected."));
        return;
    }

    // One answer per query, in order; they run one after another on the
    // session's connection, like the object tree's own queries.
    const quint64 generation = ++m_generation;
    auto results = std::make_shared<std::vector<pg::Result>>();
    auto error = std::make_shared<QString>();
    for (const QByteArray &query : queries) {
        runner->run(
            query, this, [this, generation, results, error](const pg::QueryOutcome &outcome) {
                if (generation != m_generation)
                    return; // A newer refresh is on its way.
                if (!outcome.ok() && error->isEmpty())
                    *error = outcome.error;
                results->push_back(outcome.results.empty() ? pg::Result() : outcome.results.back());
                if (results->size() < catalog::detailQueries(m_kind, m_oid).size())
                    return;
                if (!error->isEmpty()) {
                    showMessage(*error, true);
                    return;
                }
                showDetail(catalog::parseDetail(m_kind, *results));
                // A refresh with the diagram open reloads it; otherwise it
                // waits for its tab to be opened again.
                if (m_diagramTab && m_tabs->currentWidget() == m_diagramTab)
                    loadDiagram();
            });
    }
}

void ObjectPage::showDetail(const catalog::ObjectDetail &detail)
{
    m_detail = detail;
    if (detail.title.isEmpty()) {
        showMessage(tr("%1 is not there any more.").arg(m_name), true);
        return;
    }

    const QString current = m_tabs->tabText(m_tabs->currentIndex());
    m_tabs->clear();
    // clear() deletes no pages, it leaves them in the tab widget's stack;
    // take these two back.
    m_definition->setParent(this);
    m_definition->hide();
    if (m_diagramTab) {
        m_diagramTab->setParent(this);
        m_diagramTab->hide();
    }
    m_heading->setText(detail.title + QStringLiteral("  ·  ") + detail.subtitle);
    m_message->hide();
    m_tabs->show();

    m_tabs->addTab(viewOf(detail.properties, m_tabs), detail.properties.title);
    for (const catalog::DetailTable &table : detail.tables) {
        // "Columns (5)": the count belongs where it is looked for.
        m_tabs->addTab(viewOf(table, m_tabs),
                       table.rows.empty()
                           ? table.title
                           : QStringLiteral("%1 (%2)").arg(table.title).arg(table.rows.size()));
    }
    if (!detail.definition.isEmpty()) {
        m_definition->setText(detail.definition);
        m_tabs->addTab(m_definition, tr("Definition"));
    }
    if (m_diagramTab)
        m_tabs->addTab(m_diagramTab, tr("Diagram"));

    // Back to the tab that was open before the refresh, where there is one.
    for (int tab = 0; tab < m_tabs->count(); ++tab) {
        if (m_tabs->tabText(tab) == current) {
            m_tabs->setCurrentIndex(tab);
            break;
        }
    }
    if (detail.title != m_name) {
        m_name = detail.title; // The tree's name is short; this one is full.
        Q_EMIT titleChanged();
    }
}

void ObjectPage::loadDiagram()
{
    pg::QueryRunner *runner = m_session ? m_session->runner(m_database) : nullptr;
    if (!m_diagram || !runner)
        return;

    m_diagramLoaded = true;
    const quint64 generation = m_generation;
    // What the page is about decides how much of the database is drawn.
    const catalog::ErdScope scope = m_kind == catalog::ObjectKind::Database
        ? catalog::ErdScope::Database
        : m_kind == catalog::ObjectKind::Schema ? catalog::ErdScope::Schema
                                                : catalog::ErdScope::Table;
    const std::vector<QByteArray> queries = catalog::erdQueries(scope, m_oid);
    auto results = std::make_shared<std::vector<pg::Result>>();
    for (const QByteArray &query : queries) {
        runner->run(
            query, this,
            [this, generation, results, wanted = queries.size()](const pg::QueryOutcome &outcome) {
                if (generation != m_generation || !outcome.ok())
                    return; // A newer refresh, or no diagram this time.
                results->push_back(outcome.results.empty() ? pg::Result() : outcome.results.back());
                if (results->size() < wanted)
                    return;
                // Only a table's diagram singles one table out.
                m_graph
                    = catalog::parseErd(m_kind == catalog::ObjectKind::Table ? m_oid : 0, *results);
                m_diagram->setGraph(m_graph);
            });
    }
}

void ObjectPage::exportDiagram(const QString &filter)
{
    if (!m_diagram || m_diagram->graph().isEmpty())
        return;
    QSettings settings;
    // A name made from the object's, with the suffix of the format chosen.
    static const QRegularExpression suffixOf(QStringLiteral("\\*(\\.\\w+)"));
    const QString suffix = suffixOf.match(filter).captured(1);
    QString name = m_name;
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString dir = settings.value(QStringLiteral("files/lastDirectory")).toString();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Diagram"),
                                                QDir(dir).filePath(name + suffix), filter);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += suffix;
    settings.setValue(QStringLiteral("files/lastDirectory"), QFileInfo(path).absolutePath());
    QString error;
    if (!m_diagram->saveDiagram(path, &error)) {
        QMessageBox::warning(this, tr("Export Diagram"),
                             tr("Could not save %1:\n%2").arg(path, error));
    }
}

void ObjectPage::showMessage(const QString &text, bool error)
{
    m_tabs->clear();
    m_definition->setParent(this);
    m_definition->hide();
    if (m_diagramTab) {
        m_diagramTab->setParent(this);
        m_diagramTab->hide();
        m_diagram->clear();
    }
    m_graph = {};
    m_diagramLoaded = false;
    m_tabs->hide();
    m_definition->setText(QString());
    m_message->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_message->setText(text);
    m_message->show();
}

} // namespace slonisko
