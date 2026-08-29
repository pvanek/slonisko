// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/ConnectionProfile.h"
#include "config/PasswordStore.h"

#include <QDialog>
#include <QPointer>

#include <memory>
#include <optional>

class QLineEdit;

namespace Ui {
class ConnectionDialog;
}

namespace slonisko {

class Session;

// Edits a connection profile. Passwords are shown only as "saved"; one the
// user types replaces the saved one when the dialog is accepted.
class ConnectionDialog : public QDialog
{
    Q_OBJECT

public:
    // passwords reads saved passwords for Test Connection and tells whether
    // they go to the system wallet.
    ConnectionDialog(const config::ConnectionProfile &profile, config::PasswordStore *passwords,
                     QWidget *parent = nullptr);
    ~ConnectionDialog() override;

    config::ConnectionProfile profile() const;
    void setProfile(const config::ConnectionProfile &profile);

    // What the user typed, or nullopt if the field was left alone.
    std::optional<QString> password() const;
    std::optional<QString> sshSecret() const;

    // Runs a test connection with the current settings.
    void testConnection();
    // The outcome of the last test, for tests.
    QString testStatus() const;

    void accept() override;

private:
    void browseFile(QLineEdit *edit, const QString &title);
    void updateEnabled();
    void setColor(const QString &color);
    void chooseColor();
    void startTest(const QString &password, const QString &sshSecret);
    void finishTest(bool ok, const QString &message);

    std::unique_ptr<Ui::ConnectionDialog> m_ui; // Layout in ConnectionDialog.ui.
    config::PasswordStore *m_passwords = nullptr;
    QUuid m_id;
    QString m_color;
    bool m_passwordEdited = false;
    bool m_sshSecretEdited = false;
    QPointer<Session> m_test;
};

// "18.1" for 180001, "9.6.24" for 90624.
QString formatServerVersion(int version);

} // namespace slonisko
