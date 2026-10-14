// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserModel.h"

#include "Icons.h"
#include "Session.h"
#include "catalog/Monitoring.h"
#include "config/Folders.h"

#include <QDataStream>
#include <QFont>
#include <QGuiApplication>
#include <QIODevice>
#include <QMimeData>
#include <QPalette>

#include <algorithm>
#include <functional>

namespace slonisko {

using catalog::Folder;
using catalog::MonitoringQuery;
using catalog::ObjectKind;

namespace {

const QString MimeType = QStringLiteral("application/x-slonisko-connections");

} // namespace

struct BrowserModel::Node
{
    NodeType type = NodeType::Message;
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    quint64 id = 0;

    QString name;
    QString detail;
    bool system = false;
    bool error = false;

    QUuid profileId; // Connection.
    QString database; // Inherited by everything under a Database node.
    Folder folder = Folder::Schemas;
    ObjectKind kind = ObjectKind::Table;
    catalog::Oid oid = 0;
    MonitoringQuery::Group group = MonitoringQuery::Group::DbaTools;
    int monitoring = -1;

    bool populated = false; // Children created or loaded.
    bool loading = false;
};

BrowserModel::BrowserModel(QObject *parent) : QAbstractItemModel(parent) { }

BrowserModel::~BrowserModel() = default;

// Profiles and folders.

void BrowserModel::setProfiles(const std::vector<config::ConnectionProfile> &profiles,
                               const QStringList &folders, const QStringList &order)
{
    beginResetModel();
    for (auto &n : m_roots)
        forget(n.get());
    m_roots.clear();
    m_profiles.clear();
    m_connectionNodes.clear();
    endResetModel();

    m_loading = true;
    for (int i = 0; i < order.size(); ++i)
        m_loadingOrder.insert(order[i], i);
    for (const QString &f : folders)
        addFolder(f);
    for (const auto &p : profiles)
        addProfile(p);
    m_loadingOrder.clear();
    m_loading = false;

    for (const auto &p : m_profiles) {
        if (session(p.id))
            onSessionStateChanged(p.id); // Re-populate connected ones.
        else
            m_sessions.remove(p.id);
    }
}

void BrowserModel::addProfile(const config::ConnectionProfile &profile)
{
    m_profiles.push_back(profile);
    m_profiles.back().folder = config::normalizedFolder(profile.folder);

    auto node = std::make_unique<Node>();
    node->type = NodeType::Connection;
    node->id = m_nextId++;
    node->name = profile.displayName();
    node->profileId = profile.id;
    m_nodes.insert(node->id, node.get());
    m_connectionNodes.insert(profile.id, node.get());
    insertNode(ensureCategory(m_profiles.back().folder), std::move(node));
}

void BrowserModel::updateProfile(const config::ConnectionProfile &profile)
{
    Node *node = m_connectionNodes.value(profile.id);
    const auto it = std::ranges::find(m_profiles, profile.id, &config::ConnectionProfile::id);
    if (!node || it == m_profiles.end())
        return;
    *it = profile;
    it->folder = config::normalizedFolder(profile.folder);
    node->name = profile.displayName();
    moveNode(node, ensureCategory(it->folder));
    const QModelIndex index = indexOf(node);
    Q_EMIT dataChanged(index, index);
}

void BrowserModel::removeProfile(const QUuid &id)
{
    Node *node = m_connectionNodes.value(id);
    if (!node)
        return;
    setSession(id, nullptr);
    m_connectionNodes.remove(id);
    removeNode(node);
    std::erase_if(m_profiles, [&](const config::ConnectionProfile &p) { return p.id == id; });
}

const config::ConnectionProfile *BrowserModel::profile(const QUuid &id) const
{
    const auto it = std::ranges::find(m_profiles, id, &config::ConnectionProfile::id);
    return it == m_profiles.end() ? nullptr : &*it;
}

QModelIndex BrowserModel::profileIndex(const QUuid &id) const
{
    return indexOf(m_connectionNodes.value(id));
}

void BrowserModel::addFolder(const QString &path)
{
    ensureCategory(path);
}

void BrowserModel::moveFolder(const QString &folder, const QString &to)
{
    const QString from = config::normalizedFolder(folder);
    const QString dest = config::normalizedFolder(to);
    Node *source = category(from);
    // A folder cannot go into itself.
    if (!source || config::isWithinFolder(dest, from))
        return;

    for (auto &p : m_profiles)
        p.folder = config::movedFolder(p.folder, from, dest);

    if (dest.isEmpty()) {
        mergeCategory(source, nullptr);
    } else if (Node *existing = category(dest)) {
        mergeCategory(source, existing);
    } else {
        Node *parent = ensureCategory(config::parentFolder(dest));
        source->name = config::folderName(dest);
        moveNode(source, parent);
        const QModelIndex index = indexOf(source);
        Q_EMIT dataChanged(index, index);
    }
}

void BrowserModel::removeFolder(const QString &folder)
{
    moveFolder(folder, config::parentFolder(folder));
}

QStringList BrowserModel::folders() const
{
    QStringList out;
    std::function<void(const Node *)> walk = [&](const Node *parent) {
        for (const auto &n : childrenOf(parent)) {
            if (n->type != NodeType::Category)
                continue;
            out << pathOf(n.get());
            walk(n.get());
        }
    };
    walk(nullptr);
    return out;
}

QModelIndex BrowserModel::folderIndex(const QString &path) const
{
    return indexOf(category(path));
}

QString BrowserModel::pathOf(const Node *category) const
{
    QStringList names;
    for (const Node *n = category; n; n = n->parent)
        names.prepend(n->name);
    return names.join(QLatin1Char('/'));
}

BrowserModel::Node *BrowserModel::childCategory(const Node *parent, const QString &name) const
{
    for (const auto &n : childrenOf(parent)) {
        if (n->type == NodeType::Category && n->name == name)
            return n.get();
    }
    return nullptr;
}

BrowserModel::Node *BrowserModel::category(const QString &path) const
{
    Node *node = nullptr;
    for (const QString &name : config::folderNames(path)) {
        node = childCategory(node, name);
        if (!node)
            return nullptr;
    }
    return node;
}

BrowserModel::Node *BrowserModel::ensureCategory(const QString &path)
{
    Node *node = nullptr;
    for (const QString &name : config::folderNames(path)) {
        Node *next = childCategory(node, name);
        if (!next) {
            auto n = std::make_unique<Node>();
            n->type = NodeType::Category;
            n->parent = node;
            n->id = m_nextId++;
            n->name = name;
            n->populated = true;
            next = n.get();
            m_nodes.insert(n->id, next);
            insertNode(node, std::move(n));
        }
        node = next;
    }
    return node;
}

QString BrowserModel::orderKey(const Node *node) const
{
    if (node->type == NodeType::Category)
        return QStringLiteral("folder:") + pathOf(node);
    return QStringLiteral("connection:") + node->profileId.toString(QUuid::WithoutBraces);
}

int BrowserModel::loadedRow(const Node *parent, const Node *node) const
{
    auto before = [this](const Node *a, const Node *b) {
        const int ra = m_loadingOrder.value(orderKey(a), -1);
        const int rb = m_loadingOrder.value(orderKey(b), -1);
        if (ra >= 0 || rb >= 0)
            return rb < 0 || (ra >= 0 && ra < rb);
        // Neither stored: folders first, then connections; each by name.
        if ((a->type == NodeType::Category) != (b->type == NodeType::Category))
            return a->type == NodeType::Category;
        return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
    };
    int row = 0;
    for (const auto &n : childrenOf(parent)) {
        if (n.get() != node && before(n.get(), node))
            ++row;
    }
    return row;
}

void BrowserModel::insertNode(Node *parent, std::unique_ptr<Node> node)
{
    node->parent = parent;
    auto &siblings = childrenOf(parent);
    const int row = m_loading ? loadedRow(parent, node.get()) : int(siblings.size());
    beginInsertRows(indexOf(parent), row, row);
    siblings.insert(siblings.begin() + row, std::move(node));
    endInsertRows();
}

void BrowserModel::moveNode(Node *node, Node *parent, int row)
{
    auto &from = childrenOf(node->parent);
    const auto it = std::ranges::find(from, node, &std::unique_ptr<Node>::get);
    const int source = int(it - from.begin());
    const bool sameParent = node->parent == parent;
    const int others = int(childrenOf(parent).size()) - (sameParent ? 1 : 0);
    if (row < 0)
        row = sameParent ? source : others;
    row = std::min(row, others);
    if (sameParent && row == source)
        return;

    // The destination row counts the node itself while it is still there.
    const int destination = sameParent && row > source ? row + 1 : row;
    if (!beginMoveRows(indexOf(node->parent), source, source, indexOf(parent), destination))
        return;
    std::unique_ptr<Node> taken = std::move(*it);
    from.erase(it);
    taken->parent = parent;
    auto &to = childrenOf(parent);
    to.insert(to.begin() + row, std::move(taken));
    endMoveRows();
}

void BrowserModel::setRow(const QModelIndex &index, int row)
{
    Node *node = nodeOf(index);
    if (node && (node->type == NodeType::Category || node->type == NodeType::Connection))
        moveNode(node, node->parent, std::max(row, 0));
}

QStringList BrowserModel::order() const
{
    QStringList out;
    std::function<void(const Node *)> walk = [&](const Node *parent) {
        for (const auto &n : childrenOf(parent)) {
            if (n->type != NodeType::Category && n->type != NodeType::Connection)
                continue;
            out << orderKey(n.get());
            if (n->type == NodeType::Category)
                walk(n.get());
        }
    };
    walk(nullptr);
    return out;
}

void BrowserModel::removeNode(Node *node)
{
    auto &siblings = childrenOf(node->parent);
    const auto it = std::ranges::find(siblings, node, &std::unique_ptr<Node>::get);
    const int row = int(it - siblings.begin());
    beginRemoveRows(indexOf(node->parent), row, row);
    forget(node);
    siblings.erase(it);
    endRemoveRows();
}

void BrowserModel::mergeCategory(Node *source, Node *target)
{
    while (!source->children.empty()) {
        Node *child = source->children.front().get();
        Node *existing
            = child->type == NodeType::Category ? childCategory(target, child->name) : nullptr;
        if (existing)
            mergeCategory(child, existing);
        else
            moveNode(child, target);
    }
    removeNode(source);
}

// Sessions.

void BrowserModel::setSession(const QUuid &profile, Session *session)
{
    if (Session *old = m_sessions.value(profile))
        old->disconnect(this);
    if (session) {
        m_sessions.insert(profile, session);
        connect(session, &Session::stateChanged, this,
                [this, profile] { onSessionStateChanged(profile); });
    } else {
        m_sessions.remove(profile);
    }
    onSessionStateChanged(profile);
}

Session *BrowserModel::session(const QUuid &profile) const
{
    return m_sessions.value(profile);
}

Session *BrowserModel::sessionOf(const QModelIndex &index) const
{
    const Node *connection = connectionOf(nodeOf(index));
    return connection ? session(connection->profileId) : nullptr;
}

void BrowserModel::onSessionStateChanged(const QUuid &profile)
{
    const QModelIndex index = profileIndex(profile);
    if (!index.isValid())
        return;
    Node *node = nodeOf(index);
    Session *s = session(profile);

    clearChildren(node);
    node->populated = false;
    if (s && s->state() == Session::State::Connected)
        populateConnection(node);
    Q_EMIT dataChanged(index, index);
}

void BrowserModel::populateConnection(Node *connection)
{
    const config::ConnectionProfile *p = profile(connection->profileId);
    Session *s = session(connection->profileId);
    if (!p || !s)
        return;

    std::vector<std::unique_ptr<Node>> children;
    if (p->showAllDatabases) {
        children.push_back(message(connection, tr("Loading…"), false));
        s->runner()->run(
            catalog::folderQuery(Folder::Databases), this,
            [this, id = connection->id](const pg::QueryOutcome &o) { databasesLoaded(id, o); });
    } else {
        auto db = std::make_unique<Node>();
        db->type = NodeType::Database;
        db->parent = connection;
        db->id = m_nextId++;
        db->name = db->database = p->database;
        db->kind = ObjectKind::Database;
        m_nodes.insert(db->id, db.get());
        children.push_back(std::move(db));
    }
    for (auto group : {MonitoringQuery::Group::DbaTools, MonitoringQuery::Group::SystemInfo}) {
        auto node = std::make_unique<Node>();
        node->type = NodeType::Group;
        node->parent = connection;
        node->id = m_nextId++;
        node->group = group;
        node->name
            = group == MonitoringQuery::Group::DbaTools ? tr("DBA Tools") : tr("System Info");
        m_nodes.insert(node->id, node.get());
        children.push_back(std::move(node));
    }
    connection->populated = true;
    append(connection, std::move(children));
}

void BrowserModel::databasesLoaded(quint64 id, const pg::QueryOutcome &outcome)
{
    Node *connection = m_nodes.value(id);
    if (!connection)
        return;

    // Replace the "Loading…" message, which comes before the groups.
    const auto msg = std::ranges::find(connection->children, NodeType::Message,
                                       [](const auto &n) { return n->type; });
    if (msg != connection->children.end()) {
        const int row = int(msg - connection->children.begin());
        beginRemoveRows(indexOf(connection), row, row);
        forget(msg->get());
        connection->children.erase(msg);
        endRemoveRows();
    }

    std::vector<std::unique_ptr<Node>> databases;
    if (!outcome.ok()) {
        databases.push_back(message(connection, outcome.error, true));
    } else {
        for (const catalog::DbObject &o :
             catalog::parseFolder(Folder::Databases, outcome.results.back())) {
            auto db = std::make_unique<Node>();
            db->type = NodeType::Database;
            db->parent = connection;
            db->id = m_nextId++;
            db->name = db->database = o.name;
            db->kind = ObjectKind::Database;
            db->oid = o.oid;
            m_nodes.insert(db->id, db.get());
            databases.push_back(std::move(db));
        }
    }
    if (databases.empty())
        return;
    beginInsertRows(indexOf(connection), 0, int(databases.size()) - 1);
    connection->children.insert(connection->children.begin(),
                                std::make_move_iterator(databases.begin()),
                                std::make_move_iterator(databases.end()));
    endInsertRows();
}

// Lazy loading.

bool BrowserModel::canFetchMore(const QModelIndex &parent) const
{
    const Node *node = nodeOf(parent);
    if (!node || node->populated || node->loading)
        return false;
    switch (node->type) {
    case NodeType::Database:
    case NodeType::Group:
    case NodeType::Folder:
        return true;
    case NodeType::Object:
        return !catalog::foldersOf(node->kind).empty();
    default:
        return false;
    }
}

void BrowserModel::fetchMore(const QModelIndex &parent)
{
    if (!canFetchMore(parent))
        return;
    // Views may call this while handling another change, like rows being
    // inserted; add the children once that is over.
    Node *node = nodeOf(parent);
    node->loading = true;
    QMetaObject::invokeMethod(
        this,
        [this, id = node->id] {
            Node *n = m_nodes.value(id);
            if (!n || !n->loading)
                return; // Gone, or refreshed meanwhile.
            n->loading = false;
            if (n->type == NodeType::Folder)
                loadFolder(n);
            else
                addStaticChildren(n);
        },
        Qt::QueuedConnection);
}

void BrowserModel::addStaticChildren(Node *node)
{
    std::vector<std::unique_ptr<Node>> children;
    auto folder = [&](Folder f) {
        auto n = std::make_unique<Node>();
        n->type = NodeType::Folder;
        n->parent = node;
        n->id = m_nextId++;
        n->folder = f;
        n->name = catalog::folderTitle(f);
        n->database = node->database;
        m_nodes.insert(n->id, n.get());
        children.push_back(std::move(n));
    };

    if (node->type == NodeType::Group) {
        const auto &queries = catalog::monitoringQueries();
        for (int i = 0; i < int(queries.size()); ++i) {
            if (queries[std::size_t(i)].group != node->group)
                continue;
            auto n = std::make_unique<Node>();
            n->type = NodeType::Monitoring;
            n->parent = node;
            n->id = m_nextId++;
            n->monitoring = i;
            n->name = queries[std::size_t(i)].title;
            m_nodes.insert(n->id, n.get());
            children.push_back(std::move(n));
        }
        if (node->group == MonitoringQuery::Group::DbaTools) {
            folder(Folder::Roles);
            folder(Folder::Tablespaces);
        }
    } else {
        for (Folder f : catalog::foldersOf(node->kind))
            folder(f);
    }
    node->populated = true;
    append(node, std::move(children));
}

void BrowserModel::loadFolder(Node *folder)
{
    Session *s = session(connectionOf(folder)->profileId);
    pg::QueryRunner *runner = s ? s->runner(folder->database) : nullptr;
    if (!runner) {
        folder->populated = true;
        std::vector<std::unique_ptr<Node>> msg;
        msg.push_back(message(folder, tr("Not connected"), true));
        append(folder, std::move(msg));
        return;
    }

    folder->loading = true;
    std::vector<std::unique_ptr<Node>> msg;
    msg.push_back(message(folder, tr("Loading…"), false));
    append(folder, std::move(msg));

    const catalog::Oid owner = folder->parent->type == NodeType::Object ? folder->parent->oid : 0;
    runner->run(catalog::folderQuery(folder->folder, owner), this,
                [this, id = folder->id](const pg::QueryOutcome &o) { folderLoaded(id, o); });
}

void BrowserModel::folderLoaded(quint64 id, const pg::QueryOutcome &outcome)
{
    Node *folder = m_nodes.value(id);
    if (!folder)
        return; // Refreshed, disconnected or removed in the meantime.

    clearChildren(folder);
    folder->loading = false;
    folder->populated = true;

    std::vector<std::unique_ptr<Node>> children;
    if (!outcome.ok()) {
        children.push_back(message(folder, outcome.error, true));
    } else {
        for (const catalog::DbObject &o :
             catalog::parseFolder(folder->folder, outcome.results.back())) {
            auto n = std::make_unique<Node>();
            n->type = NodeType::Object;
            n->parent = folder;
            n->id = m_nextId++;
            n->kind = o.kind;
            n->oid = o.oid;
            n->name = o.name;
            n->detail = o.detail;
            n->system = o.system;
            n->database = folder->database;
            m_nodes.insert(n->id, n.get());
            children.push_back(std::move(n));
        }
    }
    append(folder, std::move(children));
    const QModelIndex index = indexOf(folder);
    Q_EMIT dataChanged(index, index);
}

void BrowserModel::refresh(const QModelIndex &index)
{
    Node *node = nodeOf(index);
    if (!node)
        return;
    if (node->type == NodeType::Connection) {
        onSessionStateChanged(node->profileId);
        return;
    }
    if (node->type == NodeType::Category || node->type == NodeType::Monitoring
        || node->type == NodeType::Message)
        return;

    const bool wasPopulated = node->populated || node->loading;
    clearChildren(node);
    node->populated = false;
    node->loading = false;
    // A new id, so a load still in flight is ignored when it arrives.
    m_nodes.remove(node->id);
    node->id = m_nextId++;
    m_nodes.insert(node->id, node);

    if (wasPopulated)
        fetchMore(index);
}

// Tree structure.

BrowserModel::Node *BrowserModel::nodeOf(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : nullptr;
}

QModelIndex BrowserModel::indexOf(const Node *node) const
{
    if (!node)
        return {};
    const auto &siblings = childrenOf(node->parent);
    const auto it = std::ranges::find(siblings, node, &std::unique_ptr<Node>::get);
    return it == siblings.end()
        ? QModelIndex()
        : createIndex(int(it - siblings.begin()), 0, const_cast<Node *>(node));
}

BrowserModel::Node *BrowserModel::connectionOf(Node *node) const
{
    while (node && node->type != NodeType::Connection)
        node = node->parent;
    return node;
}

std::vector<std::unique_ptr<BrowserModel::Node>> &BrowserModel::childrenOf(Node *node)
{
    return node ? node->children : m_roots;
}

const std::vector<std::unique_ptr<BrowserModel::Node>> &
BrowserModel::childrenOf(const Node *node) const
{
    return node ? node->children : m_roots;
}

std::unique_ptr<BrowserModel::Node> BrowserModel::message(Node *parent, const QString &text,
                                                          bool error)
{
    auto n = std::make_unique<Node>();
    n->type = NodeType::Message;
    n->parent = parent;
    n->id = m_nextId++;
    n->name = text;
    n->error = error;
    m_nodes.insert(n->id, n.get());
    return n;
}

void BrowserModel::append(Node *parent, std::vector<std::unique_ptr<Node>> children)
{
    if (children.empty())
        return;
    const int first = int(parent->children.size());
    beginInsertRows(indexOf(parent), first, first + int(children.size()) - 1);
    for (auto &c : children)
        parent->children.push_back(std::move(c));
    endInsertRows();
}

void BrowserModel::clearChildren(Node *node)
{
    if (node->children.empty())
        return;
    beginRemoveRows(indexOf(node), 0, int(node->children.size()) - 1);
    for (auto &c : node->children)
        forget(c.get());
    node->children.clear();
    endRemoveRows();
}

void BrowserModel::forget(Node *node)
{
    m_nodes.remove(node->id);
    for (auto &c : node->children)
        forget(c.get());
}

// QAbstractItemModel.

QModelIndex BrowserModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0)
        return {};
    const auto &children = childrenOf(nodeOf(parent));
    if (row >= int(children.size()))
        return {};
    return createIndex(row, 0, children[std::size_t(row)].get());
}

QModelIndex BrowserModel::parent(const QModelIndex &child) const
{
    const Node *node = nodeOf(child);
    return node ? indexOf(node->parent) : QModelIndex();
}

int BrowserModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    return int(childrenOf(nodeOf(parent)).size());
}

int BrowserModel::columnCount(const QModelIndex &) const
{
    return 1;
}

bool BrowserModel::hasChildren(const QModelIndex &parent) const
{
    const Node *node = nodeOf(parent);
    if (!node)
        return !m_roots.empty();
    if (!node->children.empty())
        return true;
    switch (node->type) {
    case NodeType::Database:
    case NodeType::Group:
        return !node->populated;
    case NodeType::Folder:
        return !node->populated;
    case NodeType::Object:
        return !node->populated && !catalog::foldersOf(node->kind).empty();
    default:
        return false;
    }
}

Qt::ItemFlags BrowserModel::flags(const QModelIndex &index) const
{
    const Node *node = nodeOf(index);
    if (!node)
        return Qt::ItemIsDropEnabled; // The top level.
    switch (node->type) {
    case NodeType::Message:
        return Qt::ItemIsEnabled;
    case NodeType::Category:
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled
            | Qt::ItemIsDropEnabled;
    case NodeType::Connection:
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    default:
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
}

QVariant BrowserModel::data(const QModelIndex &index, int role) const
{
    const Node *node = nodeOf(index);
    if (!node)
        return {};

    const config::ConnectionProfile *p
        = node->type == NodeType::Connection ? profile(node->profileId) : nullptr;
    const Session *s = node->type == NodeType::Connection ? session(node->profileId) : nullptr;
    const Session::State state = s ? s->state() : Session::State::Disconnected;

    switch (role) {
    case Qt::DisplayRole:
        return node->name;
    case DetailRole:
        if (node->type == NodeType::Connection) {
            if (state == Session::State::Connecting)
                return tr("connecting…");
            if (state == Session::State::Failed)
                return tr("failed");
            return {};
        }
        return node->detail;
    case Qt::ToolTipRole:
        switch (node->type) {
        case NodeType::Connection: {
            QString tip = p
                ? QStringLiteral("%1@%2:%3/%4").arg(p->user, p->host).arg(p->port).arg(p->database)
                : QString();
            if (p && p->ssh.enabled)
                tip += tr("\nvia SSH %1").arg(p->ssh.host);
            if (state == Session::State::Connected)
                tip += tr("\nConnected, server version %1").arg(s->serverVersion());
            else if (state == Session::State::Failed)
                tip += QLatin1Char('\n') + s->errorMessage();
            return tip;
        }
        case NodeType::Object:
            return node->detail.isEmpty()
                ? catalog::kindName(node->kind)
                : catalog::kindName(node->kind) + QStringLiteral(": ") + node->detail;
        case NodeType::Monitoring:
            return catalog::monitoringQueries()[std::size_t(node->monitoring)].description;
        case NodeType::Message:
            return node->name;
        default:
            return {};
        }
    case Qt::DecorationRole:
        switch (node->type) {
        case NodeType::Category:
            return Icons::folder();
        case NodeType::Connection:
            return Icons::connection(p ? p->color : QString(), state == Session::State::Connected);
        case NodeType::Database:
            return Icons::object(ObjectKind::Database);
        case NodeType::Folder:
        case NodeType::Group:
            return Icons::folder();
        case NodeType::Object:
            return Icons::object(node->kind);
        case NodeType::Monitoring:
            return Icons::monitoring();
        case NodeType::Message:
            return node->error ? Icons::error() : QVariant();
        }
        return {};
    case Qt::FontRole:
        if (node->type == NodeType::Connection && state == Session::State::Connected) {
            QFont font;
            font.setBold(true);
            return font;
        }
        if (node->type == NodeType::Message && !node->error) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        return {};
    case Qt::ForegroundRole:
        if (node->error || (node->type == NodeType::Connection && state == Session::State::Failed))
            return QColor(Qt::red);
        if (node->system || node->type == NodeType::Message)
            return QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text);
        return {};
    case NodeTypeRole:
        return QVariant::fromValue(node->type);
    case ProfileIdRole: {
        const Node *c = connectionOf(const_cast<Node *>(node));
        return c ? QVariant::fromValue(c->profileId) : QVariant();
    }
    case DatabaseRole:
        return node->database;
    case ObjectKindRole:
        return node->type == NodeType::Object || node->type == NodeType::Database
            ? QVariant::fromValue(node->kind)
            : QVariant();
    case OidRole:
        return node->oid;
    case MonitoringRole:
        return node->monitoring;
    case ErrorRole:
        return node->error;
    case LoadedRole:
        return node->populated && !node->loading;
    case FolderRole:
        if (node->type == NodeType::Category)
            return pathOf(node);
        return p ? p->folder : QVariant();
    default:
        return {};
    }
}

// Drag and drop.

QStringList BrowserModel::mimeTypes() const
{
    return {MimeType};
}

QMimeData *BrowserModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUuid> profiles;
    QStringList folders;
    for (const QModelIndex &index : indexes) {
        const Node *node = nodeOf(index);
        if (node && node->type == NodeType::Connection)
            profiles << node->profileId;
        else if (node && node->type == NodeType::Category)
            folders << pathOf(node);
    }
    if (profiles.isEmpty() && folders.isEmpty())
        return nullptr;

    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream << profiles << folders;
    auto *data = new QMimeData;
    data->setData(MimeType, encoded);
    return data;
}

std::optional<QString> BrowserModel::dropFolder(const QModelIndex &parent) const
{
    const Node *node = nodeOf(parent);
    if (!node)
        return QString();
    if (node->type == NodeType::Category)
        return pathOf(node);
    return std::nullopt;
}

bool BrowserModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int, int,
                                   const QModelIndex &parent) const
{
    const std::optional<QString> to = dropFolder(parent);
    if (!data || action != Qt::MoveAction || !to || !data->hasFormat(MimeType))
        return false;
    QList<QUuid> profiles;
    QStringList folders;
    QDataStream stream(data->data(MimeType));
    stream >> profiles >> folders;
    return std::ranges::none_of(folders,
                                [&](const QString &f) { return config::isWithinFolder(*to, f); });
}

bool BrowserModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    QList<QUuid> profiles;
    QStringList folders;
    QDataStream stream(data->data(MimeType));
    stream >> profiles >> folders;
    // The row as it will be once the dragged ones have left it.
    if (row >= 0) {
        const int before = row;
        for (int r = 0; r < before && r < rowCount(parent); ++r) {
            const Node *n = nodeOf(index(r, 0, parent));
            if ((n->type == NodeType::Connection && profiles.contains(n->profileId))
                || (n->type == NodeType::Category && folders.contains(pathOf(n))))
                --row;
        }
    }
    Q_EMIT moveRequested(profiles, folders, *dropFolder(parent), row);
    return true;
}

Qt::DropActions BrowserModel::supportedDragActions() const
{
    return Qt::MoveAction;
}

Qt::DropActions BrowserModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

} // namespace slonisko
