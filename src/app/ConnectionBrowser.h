// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Session.h"
#include "catalog/Objects.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QWidget>

#include <functional>
#include <map>
#include <optional>

class QAction;
class QSettings;
class QTreeView;

namespace slonisko {

class BrowserModel;

// The connection and object browser: saved connections, and for connected
// ones their databases and objects, DBA tools and system information.
class ConnectionBrowser : public QWidget
{
    Q_OBJECT

public:
    // useWallet false keeps passwords in the settings file, e.g. in tests.
    explicit ConnectionBrowser(QSettings &settings, bool useWallet = true,
                               QWidget *parent = nullptr);
    ~ConnectionBrowser() override;

    BrowserModel *model() const { return m_model; }
    QTreeView *view() const { return m_view; }
    QAction *newConnectionAction() const { return m_new; }

    void connectProfile(const QUuid &id);
    // Disconnects, first asking if editors on the connection have
    // transactions open or statements running. False if the user said no.
    bool disconnectProfile(const QUuid &id, bool ask = true);
    // Drops the session's connections and makes them again, asking the same
    // way; for a connection the network left hanging. Editors and pages on
    // it stay on it. Without a session, connects.
    bool reconnectProfile(const QUuid &id, bool ask = true);
    // How to ask; tests answer instead of a message box.
    void setConfirm(std::function<bool(const QString &question)> confirm)
    {
        m_confirm = std::move(confirm);
    }

    std::vector<Session *> connectedSessions() const;
    // The session of the selected node, and its database (empty: the
    // profile's), or null.
    Session *currentSession(QString *database = nullptr) const;

Q_SIGNALS:
    // A session connected or went away.
    void sessionsChanged();
    // "Open SQL Editor" on a connection or database.
    void editorRequested(slonisko::Session *session, const QString &database);
    // A DBA or System Info item was opened.
    void monitoringRequested(slonisko::Session *session, const QString &title,
                             const QByteArray &sql);
    // An object whose details can be shown was opened.
    void objectRequested(slonisko::Session *session, const QString &database,
                         slonisko::catalog::ObjectKind kind, unsigned int oid, const QString &name);

private:
    void createActions();
    void updateActions();
    void showContextMenu(const QPoint &pos);
    void onActivated(const QModelIndex &index);
    QUuid currentProfile() const;

    // Opens the details page of the node at index, if it has one.
    void showDetails(const QModelIndex &index);
    void newConnection();
    void editConnection();
    void duplicateConnection();
    void deleteConnection();
    void refreshCurrent();
    bool editProfile(config::ConnectionProfile &profile, std::optional<QString> &password,
                     std::optional<QString> &sshSecret);
    void storeSecrets(const config::ConnectionProfile &profile,
                      const std::optional<QString> &password,
                      const std::optional<QString> &sshSecret);

    // Reads saved passwords and asks for the others; done gets nullopt if
    // the user cancelled.
    void credentials(const config::ConnectionProfile &profile,
                     std::function<void(std::optional<Session::Credentials>)> done);
    void secret(const config::ConnectionProfile &profile, config::PasswordStore::Secret which,
                config::PasswordMode mode, const QString &prompt,
                std::function<void(std::optional<QString>)> done);
    void onSessionStateChanged(const QUuid &id, Session::State state);
    // Whether to go on although editors on session have work in progress,
    // which doing (e.g. "Disconnecting") rolls back.
    bool confirmBusy(const Session *session, const QString &title, const QString &doing);

    config::ProfileStore m_profiles;
    config::PasswordStore *m_passwords = nullptr;
    BrowserModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    std::map<QUuid, Session *> m_sessions;

    QAction *m_new = nullptr;
    QAction *m_edit = nullptr;
    QAction *m_duplicate = nullptr;
    QAction *m_delete = nullptr;
    QAction *m_connect = nullptr;
    QAction *m_disconnect = nullptr;
    QAction *m_reconnect = nullptr;
    QAction *m_refresh = nullptr;
    QAction *m_openEditor = nullptr;
    QAction *m_showDetails = nullptr;
    std::function<bool(const QString &)> m_confirm;
};

} // namespace slonisko
