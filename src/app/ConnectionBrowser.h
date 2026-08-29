// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Session.h"
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
    explicit ConnectionBrowser(QSettings &settings, QWidget *parent = nullptr);
    ~ConnectionBrowser() override;

    BrowserModel *model() const { return m_model; }
    QTreeView *view() const { return m_view; }
    QAction *newConnectionAction() const { return m_new; }

    void connectProfile(const QUuid &id);
    void disconnectProfile(const QUuid &id);

Q_SIGNALS:
    // A DBA or System Info item was opened.
    void monitoringRequested(pg::QueryRunner *runner, const QString &title, const QByteArray &sql);

private:
    void createActions();
    void updateActions();
    void showContextMenu(const QPoint &pos);
    void onActivated(const QModelIndex &index);
    QUuid currentProfile() const;

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
    QAction *m_refresh = nullptr;
};

} // namespace slonisko
