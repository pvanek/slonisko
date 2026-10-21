// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"

#include "BrowserDelegate.h"
#include "BrowserModel.h"
#include "catalog/Details.h"
#include "ConnectionDialog.h"
#include "DumpDialog.h"
#include "Icons.h"
#include "ImportDialog.h"
#include "RestoreDialog.h"
#include "Shortcuts.h"
#include "catalog/Monitoring.h"
#include "config/Folders.h"

#include <QAction>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace slonisko {

using config::PasswordMode;
using NodeType = BrowserModel::NodeType;
using Secret = config::PasswordStore::Secret;

namespace {

const QString ExpandedKey = QStringLiteral("browser/expandedFolders");

} // namespace

ConnectionBrowser::ConnectionBrowser(QSettings &settings, bool useWallet, QWidget *parent)
    : QWidget(parent), m_settings(settings), m_profiles(settings),
      m_passwords(new config::PasswordStore(settings, useWallet, this)),
      m_model(new BrowserModel(this)), m_view(new QTreeView(this))
{
    m_model->setProfiles(m_profiles.load(), m_profiles.folders(), m_profiles.order());

    m_view->setModel(m_model);
    m_view->setItemDelegate(new BrowserDelegate(m_view));
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    m_view->setExpandsOnDoubleClick(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    connect(m_view, &QTreeView::customContextMenuRequested, this,
            &ConnectionBrowser::showContextMenu);
    connect(m_view, &QTreeView::activated, this, &ConnectionBrowser::onActivated);

    m_view->setDragDropMode(QAbstractItemView::InternalMove);
    m_view->setDefaultDropAction(Qt::MoveAction);
    // Not while the view is still handling the drop.
    connect(m_model, &BrowserModel::moveRequested, this, &ConnectionBrowser::moveItems,
            Qt::QueuedConnection);
    connect(m_view, &QTreeView::expanded, this, &ConnectionBrowser::saveExpanded);
    connect(m_view, &QTreeView::collapsed, this, &ConnectionBrowser::saveExpanded);
    restoreExpanded();

    createActions();
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &ConnectionBrowser::updateActions);

    auto *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->addAction(m_new);
    toolbar->addAction(m_newFolder);
    toolbar->addAction(m_edit);
    toolbar->addSeparator();
    toolbar->addAction(m_connect);
    toolbar->addAction(m_reconnect);
    toolbar->addAction(m_disconnect);
    toolbar->addAction(m_refresh);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(m_view);
    updateActions();
}

ConnectionBrowser::~ConnectionBrowser()
{
    for (auto &[id, session] : m_sessions)
        session->disconnect(this);
}

void ConnectionBrowser::createActions()
{
    auto action = [this](const QString &icon, const QString &text, auto slot) {
        auto *a = new QAction(QIcon::fromTheme(icon), text, this);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    m_new = action(QStringLiteral("list-add"), tr("&New Connection…"),
                   &ConnectionBrowser::newConnection);
    m_edit = action(QStringLiteral("document-edit"), tr("&Edit Connection…"),
                    &ConnectionBrowser::editConnection);
    m_duplicate = action(QStringLiteral("edit-copy"), tr("D&uplicate Connection"),
                         &ConnectionBrowser::duplicateConnection);
    m_delete = action(QStringLiteral("edit-delete"), tr("&Delete Connection"), [this] {
        if (isFolderCurrent())
            deleteCurrentFolder();
        else
            deleteConnection();
    });
    m_newFolder
        = action(QStringLiteral("folder-new"), tr("New &Folder…"), &ConnectionBrowser::newFolder);
    m_renameFolder = action(QStringLiteral("edit-rename"), tr("Rena&me Folder…"),
                            &ConnectionBrowser::renameCurrentFolder);
    m_connect = action(QStringLiteral("network-connect"), tr("&Connect"),
                       [this] { connectProfile(currentProfile()); });
    m_disconnect = action(QStringLiteral("network-disconnect"), tr("Dis&connect"),
                          [this] { disconnectProfile(currentProfile()); });
    m_disconnect->setIcon(Icons::disconnect());
    m_reconnect = action({}, tr("Reco&nnect"), [this] { reconnectProfile(currentProfile()); });
    m_reconnect->setIcon(Icons::reconnect());
    m_reconnect->setToolTip(tr("Close the connection and open it again, e.g. when the network "
                               "left it hanging"));
    m_refresh = action(QStringLiteral("view-refresh"), tr("&Refresh"),
                       &ConnectionBrowser::refreshCurrent);
    m_openEditor = action(QStringLiteral("document-new"), tr("Open SQL &Editor"), [this] {
        QString database;
        if (Session *s = currentSession(&database))
            Q_EMIT editorRequested(s, database);
    });
    m_showDetails = action(QStringLiteral("dialog-information"), tr("&Details"),
                           [this] { showDetails(m_view->currentIndex()); });
    m_refresh->setShortcuts(Shortcuts::refresh());
    m_refresh->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_delete->setShortcut(QKeySequence::Delete);
    m_delete->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_renameFolder->setShortcut(Qt::Key_F2);
    m_renameFolder->setShortcutContext(Qt::WidgetWithChildrenShortcut);
}

QUuid ConnectionBrowser::currentProfile() const
{
    return m_view->currentIndex().data(BrowserModel::ProfileIdRole).toUuid();
}

QString ConnectionBrowser::currentFolder() const
{
    if (isFolderCurrent())
        return m_view->currentIndex().data(BrowserModel::FolderRole).toString();
    const config::ConnectionProfile *p = m_model->profile(currentProfile());
    return p ? p->folder : QString();
}

bool ConnectionBrowser::isFolderCurrent() const
{
    return m_view->currentIndex().data(BrowserModel::NodeTypeRole).value<NodeType>()
        == NodeType::Category;
}

void ConnectionBrowser::updateActions()
{
    const QUuid id = currentProfile();
    const Session *s = m_model->session(id);
    const bool busy = s
        && (s->state() == Session::State::Connected || s->state() == Session::State::Connecting);
    const bool folder = isFolderCurrent();
    m_edit->setEnabled(!id.isNull());
    m_duplicate->setEnabled(!id.isNull());
    m_delete->setEnabled(!id.isNull() || folder);
    m_delete->setText(folder ? tr("&Delete Folder") : tr("&Delete Connection"));
    m_renameFolder->setEnabled(folder);
    m_connect->setEnabled(!id.isNull() && !busy);
    m_disconnect->setEnabled(busy);
    m_reconnect->setEnabled(s && s->state() != Session::State::Disconnected);
    m_refresh->setEnabled(s && s->state() == Session::State::Connected);
    m_openEditor->setEnabled(s && s->state() == Session::State::Connected);
    const QModelIndex index = m_view->currentIndex();
    m_showDetails->setEnabled(
        s && s->state() == Session::State::Connected
        && catalog::hasDetails(
            index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>()));
}

void ConnectionBrowser::showDetails(const QModelIndex &index)
{
    Session *s = m_model->sessionOf(index);
    const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
    if (!s || s->state() != Session::State::Connected || !catalog::hasDetails(kind))
        return;
    Q_EMIT objectRequested(s, index.data(BrowserModel::DatabaseRole).toString(), kind,
                           index.data(BrowserModel::OidRole).toUInt(), index.data().toString());
}

std::vector<Session *> ConnectionBrowser::connectedSessions() const
{
    std::vector<Session *> out;
    for (const auto &[id, session] : m_sessions) {
        if (session->state() == Session::State::Connected)
            out.push_back(session);
    }
    std::ranges::sort(out, [](const Session *a, const Session *b) {
        return a->profile().displayName().compare(b->profile().displayName(), Qt::CaseInsensitive)
            < 0;
    });
    return out;
}

Session *ConnectionBrowser::currentSession(QString *database) const
{
    const QModelIndex index = m_view->currentIndex();
    Session *s = m_model->sessionOf(index);
    if (!s || s->state() != Session::State::Connected)
        return nullptr;
    if (database)
        *database = index.data(BrowserModel::DatabaseRole).toString();
    return s;
}

void ConnectionBrowser::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    QMenu menu(this);
    const auto type = index.data(BrowserModel::NodeTypeRole).value<NodeType>();
    if (!index.isValid()) {
        menu.addAction(m_new);
        menu.addAction(m_newFolder);
    } else if (type == NodeType::Category) {
        menu.addAction(m_new);
        menu.addAction(m_newFolder);
        menu.addSeparator();
        addMoveMenu(menu, index);
        menu.addAction(m_renameFolder);
        menu.addAction(m_delete);
    } else {
        if (type == NodeType::Connection) {
            menu.addAction(m_connect);
            menu.addAction(m_reconnect);
            menu.addAction(m_disconnect);
        }
        if (m_openEditor->isEnabled()
            && (type == NodeType::Connection || type == NodeType::Database))
            menu.addAction(m_openEditor);
        if (m_showDetails->isEnabled())
            menu.addAction(m_showDetails);
        if (m_refresh->isEnabled())
            menu.addAction(m_refresh);
        if (m_openEditor->isEnabled()) {
            const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
            const bool object = type == NodeType::Object;
            const bool database = type == NodeType::Connection || type == NodeType::Database
                || (object && kind == catalog::ObjectKind::Database);
            const bool part = object
                && (kind == catalog::ObjectKind::Schema || kind == catalog::ObjectKind::Table
                    || kind == catalog::ObjectKind::PartitionedTable);
            if (database || part) {
                menu.addSeparator();
                const QString text = !object || kind == catalog::ObjectKind::Database
                    ? tr("&Back Up…")
                    : kind == catalog::ObjectKind::Schema ? tr("&Back Up Schema…")
                                                          : tr("&Back Up Table…");
                menu.addAction(QIcon::fromTheme(QStringLiteral("document-save")), text, this,
                               [this, index] { backUp(index); });
            }
            if (database) {
                menu.addAction(QIcon::fromTheme(QStringLiteral("document-revert")), tr("Res&tore…"),
                               this, [this, index] { restore(index); });
            }
        }
        if (type == NodeType::Object && m_openEditor->isEnabled()) {
            const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
            if (kind == catalog::ObjectKind::Table
                || kind == catalog::ObjectKind::PartitionedTable) {
                menu.addSeparator();
                menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")),
                               tr("&Import CSV…"), this, [this, index] { importCsv(index); });
            } else if (kind == catalog::ObjectKind::Schema) {
                menu.addSeparator();
                menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")),
                               tr("&Import CSV into a New Table…"), this,
                               [this, index] { importCsv(index); });
            }
        }
        // Anywhere in a connection's tree, as what hangs is usually deep in
        // it: a folder that keeps loading.
        if (type != NodeType::Connection && m_reconnect->isEnabled()) {
            menu.addSeparator();
            menu.addAction(m_reconnect);
            menu.addAction(m_disconnect);
        }
        if (type == NodeType::Connection) {
            menu.addSeparator();
            menu.addAction(m_edit);
            menu.addAction(m_duplicate);
            addMoveMenu(menu, index);
            menu.addAction(m_delete);
            menu.addSeparator();
            menu.addAction(m_new);
            menu.addAction(m_newFolder);
        }
    }
    if (!menu.isEmpty())
        menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void ConnectionBrowser::onActivated(const QModelIndex &index)
{
    switch (index.data(BrowserModel::NodeTypeRole).value<NodeType>()) {
    case NodeType::Connection: {
        const QUuid id = index.data(BrowserModel::ProfileIdRole).toUuid();
        const Session *s = m_model->session(id);
        if (s && s->state() == Session::State::Connected)
            m_view->setExpanded(index, !m_view->isExpanded(index));
        else
            connectProfile(id);
        break;
    }
    case NodeType::Monitoring: {
        Session *s = m_model->sessionOf(index);
        const auto &query = catalog::monitoringQueries()[std::size_t(
            index.data(BrowserModel::MonitoringRole).toInt())];
        if (s && s->runner())
            Q_EMIT monitoringRequested(s, query.id);
        break;
    }
    case NodeType::Object: {
        // Double-click opens what an object is; folders and databases
        // expand instead, and have Details in their context menu.
        const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
        if (catalog::hasDetails(kind)) {
            showDetails(index);
            break;
        }
        m_view->setExpanded(index, !m_view->isExpanded(index));
        break;
    }
    default:
        m_view->setExpanded(index, !m_view->isExpanded(index));
        break;
    }
}

// Connecting.

void ConnectionBrowser::connectProfile(const QUuid &id)
{
    const config::ConnectionProfile *p = m_model->profile(id);
    if (!p)
        return;
    if (const Session *s = m_model->session(id);
        s && (s->state() == Session::State::Connected || s->state() == Session::State::Connecting))
        return;

    credentials(*p, [this, id](std::optional<Session::Credentials> resolved) {
        const config::ConnectionProfile *profile = m_model->profile(id);
        if (!resolved || !profile)
            return; // Cancelled, or the profile was deleted meanwhile.
        disconnectProfile(id, false); // Drops a failed session, if any.
        auto *session = new Session(*profile, *resolved, this);
        m_sessions[id] = session;
        connect(session, &Session::stateChanged, this,
                [this, id](Session::State state) { onSessionStateChanged(id, state); });
        m_model->setSession(id, session);
        session->open();
        updateActions();
    });
}

bool ConnectionBrowser::disconnectProfile(const QUuid &id, bool ask)
{
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end())
        return true;
    Session *session = it->second;
    if (ask && !confirmBusy(session, tr("Disconnect"), tr("Disconnecting")))
        return false;
    m_sessions.erase(it);
    session->disconnect(this);
    m_model->setSession(id, nullptr);
    session->close();
    session->deleteLater();
    updateActions();
    Q_EMIT sessionsChanged();
    return true;
}

bool ConnectionBrowser::reconnectProfile(const QUuid &id, bool ask)
{
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end()) {
        connectProfile(id);
        return true;
    }
    Session *session = it->second;
    if (ask && !confirmBusy(session, tr("Reconnect"), tr("Reconnecting")))
        return false;
    session->reconnect();
    updateActions();
    return true;
}

bool ConnectionBrowser::confirmBusy(const Session *session, const QString &title,
                                    const QString &doing)
{
    const int busy = session->busyConnections();
    if (busy == 0)
        return true;
    const QString question = tr("%n editor(s) on %1 have a transaction open or a statement "
                                "running. %2 rolls them back. Go on anyway?",
                                nullptr, busy)
                                 .arg(session->profile().displayName(), doing);
    if (m_confirm)
        return m_confirm(question);
    return QMessageBox::warning(this, title, question, QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel)
        == QMessageBox::Yes;
}

void ConnectionBrowser::onSessionStateChanged(const QUuid &id, Session::State state)
{
    updateActions();
    Q_EMIT sessionsChanged();
    const Session *session = m_model->session(id);
    if (state == Session::State::Connected) {
        m_view->expand(m_model->profileIndex(id));
    } else if (state == Session::State::Failed && session) {
        auto *box
            = new QMessageBox(QMessageBox::Warning, tr("Connection Failed"),
                              tr("Could not connect to %1.").arg(session->profile().displayName()),
                              QMessageBox::Ok, this);
        box->setInformativeText(session->errorMessage());
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }
}

void ConnectionBrowser::credentials(const config::ConnectionProfile &profile,
                                    std::function<void(std::optional<Session::Credentials>)> done)
{
    auto result = std::make_shared<Session::Credentials>();
    const QUuid id = profile.id;
    const config::SshTunnelSettings ssh = profile.ssh;

    auto withSsh = [this, id, ssh, result, done]() {
        using Auth = config::SshTunnelSettings::Auth;
        const config::ConnectionProfile *p = m_model->profile(id);
        if (!p || !ssh.enabled || ssh.auth == Auth::Agent
            || (ssh.auth == Auth::KeyFile && ssh.passwordMode != PasswordMode::Save)) {
            done(*result);
            return;
        }
        secret(*p, Secret::Ssh, ssh.passwordMode,
               ssh.auth == Auth::Password ? tr("SSH password for %1@%2:").arg(ssh.user, ssh.host)
                                          : tr("Passphrase for %1:").arg(ssh.keyFile),
               [result, done](std::optional<QString> s) {
                   if (!s) {
                       done(std::nullopt);
                       return;
                   }
                   result->sshSecret = *s;
                   done(*result);
               });
    };

    if (profile.passwordMode == PasswordMode::None) {
        withSsh();
        return;
    }
    secret(profile, Secret::Postgres, profile.passwordMode,
           tr("Password for %1:").arg(profile.displayName()),
           [result, withSsh, done](std::optional<QString> password) {
               if (!password) {
                   done(std::nullopt);
                   return;
               }
               result->password = *password;
               withSsh();
           });
}

void ConnectionBrowser::secret(const config::ConnectionProfile &profile, Secret which,
                               PasswordMode mode, const QString &prompt,
                               std::function<void(std::optional<QString>)> done)
{
    const QUuid id = profile.id;
    auto ask = [this, id, which, mode, prompt, done] {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Password"), prompt,
                                                   QLineEdit::Password, QString(), &ok);
        if (!ok) {
            done(std::nullopt);
            return;
        }
        if (mode == PasswordMode::Save) // Nothing was saved yet: save it now.
            m_passwords->write(id, which, text, this);
        done(text);
    };

    if (mode != PasswordMode::Save) {
        ask();
        return;
    }
    m_passwords->read(id, which, this, [ask, done](std::optional<QString> saved) {
        if (saved)
            done(*saved);
        else
            ask();
    });
}

// Profiles.

bool ConnectionBrowser::editProfile(config::ConnectionProfile &profile,
                                    std::optional<QString> &password,
                                    std::optional<QString> &sshSecret)
{
    ConnectionDialog dialog(profile, m_passwords, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    const QUuid id = profile.id;
    profile = dialog.profile();
    profile.id = id;
    password = dialog.password();
    sshSecret = dialog.sshSecret();
    return true;
}

void ConnectionBrowser::storeSecrets(const config::ConnectionProfile &profile,
                                     const std::optional<QString> &password,
                                     const std::optional<QString> &sshSecret)
{
    using Auth = config::SshTunnelSettings::Auth;
    if (profile.passwordMode != PasswordMode::Save)
        m_passwords->remove(profile.id, Secret::Postgres);
    else if (password)
        m_passwords->write(profile.id, Secret::Postgres, *password, this);

    const bool keepSsh = profile.ssh.enabled && profile.ssh.auth != Auth::Agent
        && profile.ssh.passwordMode == PasswordMode::Save;
    if (!keepSsh)
        m_passwords->remove(profile.id, Secret::Ssh);
    else if (sshSecret)
        m_passwords->write(profile.id, Secret::Ssh, *sshSecret, this);
}

void ConnectionBrowser::newConnection()
{
    config::ConnectionProfile profile;
    profile.user = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    profile.folder = currentFolder();
    std::optional<QString> password, sshSecret;
    if (!editProfile(profile, password, sshSecret))
        return;
    m_profiles.save(profile);
    storeSecrets(profile, password, sshSecret);
    m_model->addProfile(profile);
    saveOrder();
    m_view->setCurrentIndex(m_model->profileIndex(profile.id));
}

void ConnectionBrowser::editConnection()
{
    const config::ConnectionProfile *current = m_model->profile(currentProfile());
    if (!current)
        return;
    config::ConnectionProfile profile = *current;
    std::optional<QString> password, sshSecret;
    if (!editProfile(profile, password, sshSecret))
        return;
    m_profiles.save(profile);
    storeSecrets(profile, password, sshSecret);
    m_model->updateProfile(profile);
}

void ConnectionBrowser::duplicateConnection()
{
    const config::ConnectionProfile *current = m_model->profile(currentProfile());
    if (!current)
        return;
    const QUuid source = current->id;
    config::ConnectionProfile copy = *current;
    copy.id = QUuid();
    copy.name = tr("%1 (copy)").arg(current->displayName());
    m_profiles.save(copy);

    // Copy the saved passwords too.
    for (const Secret which : {Secret::Postgres, Secret::Ssh}) {
        m_passwords->read(source, which, this,
                          [this, id = copy.id, which](std::optional<QString> saved) {
                              if (saved)
                                  m_passwords->write(id, which, *saved, this);
                          });
    }
    m_model->addProfile(copy);
    saveOrder();
    m_view->setCurrentIndex(m_model->profileIndex(copy.id));
}

void ConnectionBrowser::deleteConnection()
{
    const config::ConnectionProfile *current = m_model->profile(currentProfile());
    if (!current)
        return;
    const QUuid id = current->id;
    if (QMessageBox::question(
            this, tr("Delete Connection"),
            tr("Delete the connection %1 and its saved passwords?").arg(current->displayName()))
        != QMessageBox::Yes)
        return;
    if (!disconnectProfile(id))
        return;
    m_passwords->remove(id, Secret::Postgres);
    m_passwords->remove(id, Secret::Ssh);
    m_profiles.remove(id);
    m_model->removeProfile(id);
    updateActions();
}

// Folders.

void ConnectionBrowser::addFolder(const QString &path)
{
    m_profiles.addFolder(path);
    m_model->addFolder(path);
    saveOrder();
}

bool ConnectionBrowser::renameFolder(const QString &folder, const QString &name)
{
    const QString to = config::childFolder(config::parentFolder(folder), name);
    if (to == folder)
        return true;
    if (m_model->folderIndex(to).isValid())
        return false;
    m_profiles.moveFolder(folder, to);
    m_model->moveFolder(folder, to);
    saveExpanded();
    saveOrder();
    return true;
}

void ConnectionBrowser::removeFolder(const QString &folder)
{
    m_profiles.removeFolder(folder);
    m_model->removeFolder(folder);
    saveExpanded();
    saveOrder();
    updateActions();
}

void ConnectionBrowser::moveItems(const QList<QUuid> &profiles, const QStringList &folders,
                                  const QString &to, int row)
{
    // Each goes into to first, then to its row, one after another.
    auto place = [&](const QModelIndex &index) {
        if (row >= 0 && index.isValid())
            m_model->setRow(index, row++);
    };
    for (const QUuid &id : profiles) {
        const config::ConnectionProfile *current = m_model->profile(id);
        if (!current)
            continue;
        if (current->folder != to) {
            config::ConnectionProfile profile = *current;
            profile.folder = to;
            m_profiles.save(profile);
            m_model->updateProfile(profile);
        }
        place(m_model->profileIndex(id));
    }
    for (const QString &folder : folders) {
        if (config::isWithinFolder(to, folder))
            continue;
        const QString dest = config::childFolder(to, config::folderName(folder));
        if (dest != folder) {
            m_profiles.moveFolder(folder, dest);
            m_model->moveFolder(folder, dest);
        }
        place(m_model->folderIndex(dest)); // Not there if merged into another.
    }
    saveExpanded();
    saveOrder();
}

QStringList ConnectionBrowser::moveTargets(const QModelIndex &index) const
{
    const auto type = index.data(BrowserModel::NodeTypeRole).value<NodeType>();
    const QString folder = index.data(BrowserModel::FolderRole).toString();
    if (type != NodeType::Connection && type != NodeType::Category)
        return {};
    // Where it is now is no move; a folder cannot go into itself either.
    const QString current = type == NodeType::Category ? config::parentFolder(folder) : folder;
    QStringList out;
    for (const QString &f : QStringList {QString()} + m_model->folders()) {
        if (f == current || (type == NodeType::Category && config::isWithinFolder(f, folder)))
            continue;
        out << f;
    }
    return out;
}

void ConnectionBrowser::addMoveMenu(QMenu &menu, const QModelIndex &index)
{
    const QStringList targets = moveTargets(index);
    QMenu *move = menu.addMenu(QIcon::fromTheme(QStringLiteral("go-jump")), tr("&Move to Folder"));
    move->setEnabled(!targets.isEmpty());
    const bool isFolder
        = index.data(BrowserModel::NodeTypeRole).value<NodeType>() == NodeType::Category;
    const QUuid id = index.data(BrowserModel::ProfileIdRole).toUuid();
    const QString folder = index.data(BrowserModel::FolderRole).toString();
    for (const QString &to : targets) {
        QAction *a = to.isEmpty() ? move->addAction(tr("Top Level"))
                                  : move->addAction(Icons::folder(), to);
        connect(a, &QAction::triggered, this, [this, isFolder, id, folder, to] {
            if (isFolder)
                moveItems({}, {folder}, to);
            else
                moveItems({id}, {}, to);
        });
    }
}

void ConnectionBrowser::newFolder()
{
    const QString parent = currentFolder();
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New Folder"),
        parent.isEmpty() ? tr("Folder name:") : tr("Folder name, in %1:").arg(parent),
        QLineEdit::Normal, QString(), &ok);
    const QString path = config::childFolder(parent, name);
    if (!ok || path == parent)
        return;
    addFolder(path);
    const QModelIndex index = m_model->folderIndex(path);
    m_view->expand(index.parent());
    m_view->setCurrentIndex(index);
}

void ConnectionBrowser::renameCurrentFolder()
{
    if (!isFolderCurrent())
        return;
    const QString folder = currentFolder();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Folder"), tr("Folder name:"),
                                               QLineEdit::Normal, config::folderName(folder), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Rename Folder"),
                             tr("A folder name cannot contain \"/\". Drag the folder to move "
                                "it elsewhere."));
        return;
    }
    if (!renameFolder(folder, name)) {
        QMessageBox::warning(this, tr("Rename Folder"),
                             tr("There is a folder named %1 there already.").arg(name));
        return;
    }
    m_view->setCurrentIndex(
        m_model->folderIndex(config::childFolder(config::parentFolder(folder), name)));
}

void ConnectionBrowser::deleteCurrentFolder()
{
    if (!isFolderCurrent())
        return;
    const QString folder = currentFolder();
    const QString parent = config::parentFolder(folder);
    const QString question = m_model->rowCount(m_model->folderIndex(folder)) == 0
        ? tr("Delete the folder %1?").arg(folder)
        : parent.isEmpty()
        ? tr("Delete the folder %1? What is in it moves to the top level.").arg(folder)
        : tr("Delete the folder %1? What is in it moves to %2.").arg(folder, parent);
    if (QMessageBox::question(this, tr("Delete Folder"), question) != QMessageBox::Yes)
        return;
    removeFolder(folder);
}

void ConnectionBrowser::saveExpanded()
{
    QStringList expanded;
    for (const QString &folder : m_model->folders()) {
        if (m_view->isExpanded(m_model->folderIndex(folder)))
            expanded << folder;
    }
    m_settings.setValue(ExpandedKey, expanded);
}

void ConnectionBrowser::saveOrder()
{
    m_profiles.setOrder(m_model->order());
}

void ConnectionBrowser::restoreExpanded()
{
    for (const QString &folder : m_settings.value(ExpandedKey).toStringList())
        m_view->expand(m_model->folderIndex(folder));
}

namespace {

// The schema the node at index is, or is in; empty if none.
QString schemaOf(QModelIndex index)
{
    for (; index.isValid(); index = index.parent()) {
        if (index.data(BrowserModel::NodeTypeRole).value<BrowserModel::NodeType>()
                == BrowserModel::NodeType::Object
            && index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>()
                == catalog::ObjectKind::Schema)
            return index.data().toString();
    }
    return {};
}

// The database the node at index is, or is in; empty for the profile's.
QString databaseOf(const QModelIndex &index)
{
    if (index.data(BrowserModel::NodeTypeRole).value<BrowserModel::NodeType>()
            == BrowserModel::NodeType::Object
        && index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>()
            == catalog::ObjectKind::Database)
        return index.data().toString();
    return index.data(BrowserModel::DatabaseRole).toString();
}

} // namespace

DumpDialog *ConnectionBrowser::backUp(const QModelIndex &index)
{
    Session *s = m_model->sessionOf(index);
    if (!s || s->state() != Session::State::Connected)
        return nullptr;
    const auto type = index.data(BrowserModel::NodeTypeRole).value<NodeType>();
    const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
    QString table;
    if (type == NodeType::Object) {
        if (kind == catalog::ObjectKind::Table || kind == catalog::ObjectKind::PartitionedTable)
            table = index.data().toString();
        else if (kind != catalog::ObjectKind::Schema && kind != catalog::ObjectKind::Database)
            return nullptr;
    } else if (type != NodeType::Connection && type != NodeType::Database) {
        return nullptr;
    }
    auto *dialog = new DumpDialog(s, databaseOf(index), schemaOf(index), table, &m_settings, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

RestoreDialog *ConnectionBrowser::restore(const QModelIndex &index)
{
    Session *s = m_model->sessionOf(index);
    if (!s || s->state() != Session::State::Connected)
        return nullptr;
    const auto type = index.data(BrowserModel::NodeTypeRole).value<NodeType>();
    if (type != NodeType::Connection && type != NodeType::Database
        && !(type == NodeType::Object
             && index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>()
                 == catalog::ObjectKind::Database))
        return nullptr;
    const QString database = databaseOf(index);
    auto *dialog = new RestoreDialog(s, database, &m_settings, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // What was restored shows in the browser and in completion.
    const QPersistentModelIndex where(index);
    connect(dialog, &RestoreDialog::toolFinished, this, [this, where, s, database](bool ok) {
        if (!ok)
            return;
        s->reloadSnapshot(database);
        if (where.isValid())
            m_model->refresh(where);
    });
    dialog->show();
    return dialog;
}

ImportDialog *ConnectionBrowser::importCsv(const QModelIndex &index)
{
    Session *s = m_model->sessionOf(index);
    if (!s || s->state() != Session::State::Connected
        || index.data(BrowserModel::NodeTypeRole).value<NodeType>() != NodeType::Object)
        return nullptr;
    const auto kind = index.data(BrowserModel::ObjectKindRole).value<catalog::ObjectKind>();
    QString table;
    if (kind == catalog::ObjectKind::Table || kind == catalog::ObjectKind::PartitionedTable)
        table = index.data().toString();
    else if (kind != catalog::ObjectKind::Schema)
        return nullptr;
    auto *dialog = new ImportDialog(s, databaseOf(index), schemaOf(index), table, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // A new table shows up under the schema it went into.
    const QPersistentModelIndex where(index);
    connect(dialog, &ImportDialog::imported, this, [this, where, table] {
        if (table.isEmpty() && where.isValid())
            m_model->refresh(where);
    });
    dialog->show();
    return dialog;
}

void ConnectionBrowser::refreshCurrent()
{
    const QModelIndex index = m_view->currentIndex();
    if (index.isValid())
        m_model->refresh(index);
}

} // namespace slonisko
