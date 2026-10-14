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
#include <optional>
#include <vector>

namespace slonisko {

class Session;

// The object browser: connection profiles, sorted into folders, and for
// connected ones their databases, DBA tools and system information. Objects are read from
// the server when a folder is first expanded and are never cached; refresh()
// reads them again.
class BrowserModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum class NodeType {
        Category, // A folder of connections; see config/Folders.h.
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
        // For Category nodes their path; for Connection nodes the folder
        // they are in.
        FolderRole,
    };

    explicit BrowserModel(QObject *parent = nullptr);
    ~BrowserModel() override;

    // folders may name empty ones; the profiles' folders are added. order,
    // as order() gave it, is where everything goes; what it does not know
    // follows, folders first, then connections, each by name.
    void setProfiles(const std::vector<config::ConnectionProfile> &profiles,
                     const QStringList &folders = {}, const QStringList &order = {});
    // At the end of its folder.
    void addProfile(const config::ConnectionProfile &profile);
    // Moves it to the end of its folder if that changed.
    void updateProfile(const config::ConnectionProfile &profile);
    void removeProfile(const QUuid &id);
    const config::ConnectionProfile *profile(const QUuid &id) const;

    // The same as config::ProfileStore does to the stored ones. What ends up
    // in another folder goes to its end.
    void addFolder(const QString &path);
    void moveFolder(const QString &folder, const QString &to);
    void removeFolder(const QString &folder);
    // Every folder, sorted.
    QStringList folders() const;
    QModelIndex folderIndex(const QString &path) const;
    // Moves a connection or folder to row within its folder.
    void setRow(const QModelIndex &index, int row);
    // Every connection and folder, in the order of the tree, for setProfiles.
    QStringList order() const;

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

    // Connections and folders are dragged into folders. The model only
    // tells; whoever stores the profiles moves them.
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;

Q_SIGNALS:
    // Connections and folders were dropped into the folder to, at row of
    // it as it is without them, or at its end if row is -1.
    void moveRequested(const QList<QUuid> &profiles, const QStringList &folders, const QString &to,
                       int row);

private:
    struct Node;

    Node *nodeOf(const QModelIndex &index) const;
    QModelIndex indexOf(const Node *node) const;
    Node *connectionOf(Node *node) const;
    std::vector<std::unique_ptr<Node>> &childrenOf(Node *node);
    const std::vector<std::unique_ptr<Node>> &childrenOf(const Node *node) const;
    QString pathOf(const Node *category) const;
    Node *childCategory(const Node *parent, const QString &name) const;
    Node *category(const QString &path) const;
    // The folder's node, made with the ones above it if need be; null for the
    // top level.
    Node *ensureCategory(const QString &path);
    QString orderKey(const Node *node) const;
    // Where node goes among parent's children while setProfiles() puts them
    // in the stored order, ignoring node itself.
    int loadedRow(const Node *parent, const Node *node) const;
    // At the end, or in the stored order while loading.
    void insertNode(Node *parent, std::unique_ptr<Node> node);
    // Moves node to row of parent's children, counted without it; -1 is the
    // end, or where it is if parent is its folder already.
    void moveNode(Node *node, Node *parent, int row = -1);
    void removeNode(Node *node);
    // Moves source's contents into target (null: the top level), merging
    // folders of the same name, and removes source.
    void mergeCategory(Node *source, Node *target);
    // The folder dropping on parent puts things into; nullopt if none.
    std::optional<QString> dropFolder(const QModelIndex &parent) const;
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

    std::vector<std::unique_ptr<Node>> m_roots;
    std::vector<config::ConnectionProfile> m_profiles;
    QHash<QUuid, Node *> m_connectionNodes;
    // While setProfiles() builds the tree, in the stored order, by orderKey().
    bool m_loading = false;
    QHash<QString, int> m_loadingOrder;
    QHash<QUuid, QPointer<Session>> m_sessions;
    // Live nodes by id, so late query results can find their node, or not.
    QHash<quint64, Node *> m_nodes;
    quint64 m_nextId = 1;
};

} // namespace slonisko
