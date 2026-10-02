// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"
#include "config/ConnectionProfile.h"
#include "pg/QueryRunner.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QPointer>

#include <memory>
#include <vector>

namespace slonisko {

class Session;

// The object browser: connection profiles at the top, and for connected ones
// their databases, DBA tools and system information. Objects are read from
// the server when a folder is first expanded and are never cached; refresh()
// reads them again.
class BrowserModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum class NodeType {
        Connection,
        Database,
        Folder, // Objects listed by a catalog query, e.g. Tables.
        Group, // DBA Tools or System Info.
        Object, // A database object; may hold folders, like a table.
        Monitoring, // A prepared query from catalog::monitoringQueries().
        Message, // "Loading…" or an error.
    };
    Q_ENUM(NodeType)

    enum Role {
        NodeTypeRole = Qt::UserRole + 1,
        ProfileIdRole,
        DatabaseRole, // The database a node belongs to, empty for server-wide ones.
        ObjectKindRole,
        OidRole,
        MonitoringRole, // Index into catalog::monitoringQueries().
        DetailRole, // Secondary text, like a column's type.
        ErrorRole, // For Message nodes: true if it reports an error.
        LoadedRole, // Children created, or read from the server (successfully or not).
    };

    explicit BrowserModel(QObject *parent = nullptr);
    ~BrowserModel() override;

    // Profiles are kept in the order given.
    void setProfiles(const std::vector<config::ConnectionProfile> &profiles);
    void addProfile(const config::ConnectionProfile &profile);
    void updateProfile(const config::ConnectionProfile &profile);
    void removeProfile(const QUuid &id);
    const config::ConnectionProfile *profile(const QUuid &id) const;

    // Shows the session's objects under its profile while it is connected.
    // Null detaches it. The model does not own sessions.
    void setSession(const QUuid &profile, Session *session);
    Session *session(const QUuid &profile) const;

    QModelIndex profileIndex(const QUuid &id) const;
    // Reads the node's children from the server again.
    void refresh(const QModelIndex &index);

    // The session a node's queries run in; DatabaseRole names the database.
    Session *sessionOf(const QModelIndex &index) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    struct Node;

    Node *nodeOf(const QModelIndex &index) const;
    QModelIndex indexOf(const Node *node) const;
    Node *connectionOf(Node *node) const;
    void append(Node *parent, std::vector<std::unique_ptr<Node>> children);
    void clearChildren(Node *node);
    void onSessionStateChanged(const QUuid &profile);
    void populateConnection(Node *connection);
    void addStaticChildren(Node *node);
    void loadFolder(Node *folder);
    void folderLoaded(quint64 id, const pg::QueryOutcome &outcome);
    void databasesLoaded(quint64 id, const pg::QueryOutcome &outcome);
    std::unique_ptr<Node> message(Node *parent, const QString &text, bool error);
    void forget(Node *node);

    std::vector<std::unique_ptr<Node>> m_connections;
    std::vector<config::ConnectionProfile> m_profiles;
    QHash<QUuid, QPointer<Session>> m_sessions;
    // Live nodes by id, so late query results can find their node, or not.
    QHash<quint64, Node *> m_nodes;
    quint64 m_nextId = 1;
};

} // namespace slonisko
