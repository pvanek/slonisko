// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "WorkspacePage.h"
#include "catalog/Details.h"
#include "catalog/Erd.h"

#include <QPointer>

class QLabel;
class QTabWidget;

namespace slonisko {

class CalloutLabel;
class DependencyView;

class ErdView;
class ResultTextView;
class ResultView;
class Session;

// What one database object is: its properties, the lists that belong to it
// (columns, indexes, …) and its definition, each on a tab of its own.
class ObjectPage : public WorkspacePage
{
    Q_OBJECT

public:
    ObjectPage(Session *session, const QString &database, catalog::ObjectKind kind,
               catalog::Oid oid, const QString &name, QWidget *parent = nullptr);

    QString title() const override { return m_name; }
    QColor color() const override;

    Session *session() const { return m_session; }
    catalog::Oid oid() const { return m_oid; }
    catalog::ObjectKind kind() const { return m_kind; }
    const catalog::ObjectDetail &detail() const { return m_detail; }
    QTabWidget *tabs() const { return m_tabs; }
    // The diagram of this table and its neighbours, or of the schema's or
    // the database's tables; null for kinds that have no foreign keys.
    ErdView *diagram() const { return m_diagram; }
    // What depends on the object; null for kinds nothing can depend on.
    DependencyView *dependencies() const { return m_dependencies; }
    // The first rows of a table or view; null for kinds without rows.
    ResultView *data() const { return m_data; }
    // Opens the Dependencies tab, now or once the page has loaded.
    void showDependencies();
    // Reads everything about the object again.
    void refresh();

Q_SIGNALS:
    // A neighbour in the diagram was double-clicked.
    void objectRequested(slonisko::Session *session, const QString &database,
                         slonisko::catalog::ObjectKind kind, unsigned int oid);
    // Reconnect or Disconnect was chosen for the page's connection, as in
    // the object tree, which owns it.
    void reconnectRequested(slonisko::Session *session);
    void disconnectRequested(slonisko::Session *session);

private:
    void showDetail(const catalog::ObjectDetail &detail);
    void loadDiagram();
    void loadDependencies();
    void loadData();
    // Opens the tab whose queries wait for it, if one of them is current.
    void onTabChanged();
    // Asks where to save the diagram, in the one format the filter names.
    void exportDiagram(const QString &filter);
    void showMessage(const QString &text, bool error = false);
    void onSessionState();

    QPointer<Session> m_session;
    QString m_database;
    catalog::ObjectKind m_kind;
    catalog::Oid m_oid = 0;
    QString m_name;
    catalog::ObjectDetail m_detail;

    QLabel *m_heading = nullptr;
    QAction *m_reconnect = nullptr;
    QAction *m_disconnect = nullptr;
    CalloutLabel *m_message = nullptr;
    QTabWidget *m_tabs = nullptr;
    ResultTextView *m_definition = nullptr;
    ErdView *m_diagram = nullptr;
    QWidget *m_diagramTab = nullptr; // The diagram with its own little toolbar.
    bool m_diagramLoaded = false; // Its queries run when the tab is first opened.
    catalog::ErdGraph m_graph;
    ResultView *m_data = nullptr;
    bool m_dataLoaded = false; // Waits for its tab too: a view may take long to run.
    DependencyView *m_dependencies = nullptr;
    bool m_dependenciesLoaded = false; // Also waits for its tab: it reads the whole catalog.
    bool m_wantDependencies = false; // Asked for before the tabs were there.
    quint64 m_generation = 0; // Drops the answers of an earlier refresh.
};

} // namespace slonisko
