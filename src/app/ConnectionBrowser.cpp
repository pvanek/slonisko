// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionBrowser.h"

#include "BrowserDelegate.h"
#include "BrowserModel.h"
#include "ConnectionDialog.h"
#include "catalog/Monitoring.h"

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

ConnectionBrowser::ConnectionBrowser(QSettings &settings, bool useWallet, QWidget *parent)
    : QWidget(parent), m_profiles(settings),
      m_passwords(new config::PasswordStore(settings, useWallet, this)),
      m_model(new BrowserModel(this)), m_view(new QTreeView(this))
{
    m_model->setProfiles(m_profiles.load());

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

    createActions();
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &ConnectionBrowser::updateActions);

    auto *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->addAction(m_new);
    toolbar->addAction(m_edit);
    toolbar->addSeparator();
    toolbar->addAction(m_connect);
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
    m_delete = action(QStringLiteral("edit-delete"), tr("&Delete Connection"),
                      &ConnectionBrowser::deleteConnection);
    m_connect = action(QStringLiteral("network-connect"), tr("&Connect"),
                       [this] { connectProfile(currentProfile()); });
    m_disconnect = action(QStringLiteral("network-disconnect"), tr("Dis&connect"),
                          [this] { disconnectProfile(currentProfile()); });
    m_refresh = action(QStringLiteral("view-refresh"), tr("&Refresh"),
                       &ConnectionBrowser::refreshCurrent);
    m_openEditor = action(QStringLiteral("document-new"), tr("Open SQL &Editor"), [this] {
        QString database;
        if (Session *s = currentSession(&database))
            Q_EMIT editorRequested(s, database);
    });
    m_refresh->setShortcut(QKeySequence::Refresh);
    m_refresh->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_delete->setShortcut(QKeySequence::Delete);
    m_delete->setShortcutContext(Qt::WidgetWithChildrenShortcut);
}

QUuid ConnectionBrowser::currentProfile() const
{
    return m_view->currentIndex().data(BrowserModel::ProfileIdRole).toUuid();
}

void ConnectionBrowser::updateActions()
{
    const QUuid id = currentProfile();
    const Session *s = m_model->session(id);
    const bool busy = s
        && (s->state() == Session::State::Connected || s->state() == Session::State::Connecting);
    m_edit->setEnabled(!id.isNull());
    m_duplicate->setEnabled(!id.isNull());
    m_delete->setEnabled(!id.isNull());
    m_connect->setEnabled(!id.isNull() && !busy);
    m_disconnect->setEnabled(busy);
    m_refresh->setEnabled(s && s->state() == Session::State::Connected);
    m_openEditor->setEnabled(s && s->state() == Session::State::Connected);
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
    if (!index.isValid()) {
        menu.addAction(m_new);
    } else {
        const auto type = index.data(BrowserModel::NodeTypeRole).value<NodeType>();
        if (type == NodeType::Connection) {
            menu.addAction(m_connect);
            menu.addAction(m_disconnect);
        }
        if (m_openEditor->isEnabled()
            && (type == NodeType::Connection || type == NodeType::Database))
            menu.addAction(m_openEditor);
        if (m_refresh->isEnabled())
            menu.addAction(m_refresh);
        if (type == NodeType::Connection) {
            menu.addSeparator();
            menu.addAction(m_edit);
            menu.addAction(m_duplicate);
            menu.addAction(m_delete);
            menu.addSeparator();
            menu.addAction(m_new);
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
            Q_EMIT monitoringRequested(
                s->runner(), s->profile().displayName() + QStringLiteral(": ") + query.title,
                query.sql(s->serverVersion()));
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
        disconnectProfile(id); // Drops a failed session, if any.
        auto *session = new Session(*profile, *resolved, this);
        m_sessions[id] = session;
        connect(session, &Session::stateChanged, this,
                [this, id](Session::State state) { onSessionStateChanged(id, state); });
        m_model->setSession(id, session);
        session->open();
        updateActions();
    });
}

void ConnectionBrowser::disconnectProfile(const QUuid &id)
{
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end())
        return;
    Session *session = it->second;
    m_sessions.erase(it);
    session->disconnect(this);
    m_model->setSession(id, nullptr);
    session->close();
    session->deleteLater();
    updateActions();
    Q_EMIT sessionsChanged();
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
    std::optional<QString> password, sshSecret;
    if (!editProfile(profile, password, sshSecret))
        return;
    m_profiles.save(profile);
    storeSecrets(profile, password, sshSecret);
    m_model->addProfile(profile);
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
    disconnectProfile(id);
    m_passwords->remove(id, Secret::Postgres);
    m_passwords->remove(id, Secret::Ssh);
    m_profiles.remove(id);
    m_model->removeProfile(id);
    updateActions();
}

void ConnectionBrowser::refreshCurrent()
{
    const QModelIndex index = m_view->currentIndex();
    if (index.isValid())
        m_model->refresh(index);
}

} // namespace slonisko
