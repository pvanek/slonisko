// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QUuid>

#include <functional>
#include <optional>

class QSettings;

namespace slonisko::config {

// Passwords of connection profiles, kept in the system wallet (Secret
// Service, KWallet, macOS Keychain, Windows Credential Manager) through
// QtKeychain. When that is not built in, not running or fails, they go to
// the settings file in plain text instead.
//
// Callbacks run asynchronously and are dropped if their context is deleted.
class PasswordStore : public QObject
{
    Q_OBJECT

public:
    enum class Secret { Postgres, Ssh };

    // useWallet false forces the plain-text fallback, e.g. in tests.
    explicit PasswordStore(QSettings &settings, bool useWallet = true, QObject *parent = nullptr);

    // Whether passwords go to the system wallet at all.
    bool usesWallet() const { return m_useWallet; }
    static bool walletCompiledIn();

    void read(const QUuid &profile, Secret secret, QObject *context,
              std::function<void(std::optional<QString> password)> done);
    // done(true) if it went to the wallet, false if to the settings file.
    void write(const QUuid &profile, Secret secret, const QString &password, QObject *context,
               std::function<void(bool inWallet)> done = {});
    void remove(const QUuid &profile, Secret secret);

private:
    std::optional<QString> readFallback(const QUuid &profile, Secret secret) const;
    void writeFallback(const QUuid &profile, Secret secret, const QString &password);
    void removeFallback(const QUuid &profile, Secret secret);

    QSettings &m_settings;
    bool m_useWallet = false;
};

} // namespace slonisko::config
