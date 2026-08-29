// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/PasswordStore.h"

#include <QPointer>
#include <QSettings>

#ifdef SLONISKO_HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace slonisko::config {

namespace {

QString secretName(PasswordStore::Secret secret)
{
    return secret == PasswordStore::Secret::Postgres ? QStringLiteral("postgres")
                                                     : QStringLiteral("ssh");
}

#ifdef SLONISKO_HAVE_KEYCHAIN
const QString WalletService = QStringLiteral("slonisko");

QString walletKey(const QUuid &profile, PasswordStore::Secret secret)
{
    return profile.toString(QUuid::WithoutBraces) + QLatin1Char('/') + secretName(secret);
}
#endif

// Kept inside the profile's group, so removing the profile removes it too.
QString fallbackKey(const QUuid &profile, PasswordStore::Secret secret)
{
    return QStringLiteral("connections/") + profile.toString(QUuid::WithoutBraces)
        + QStringLiteral("/passwords/") + secretName(secret);
}

// Runs f on context's thread once control returns to the event loop.
template<typename F>
void later(QObject *context, F f)
{
    QMetaObject::invokeMethod(context, std::move(f), Qt::QueuedConnection);
}

} // namespace

PasswordStore::PasswordStore(QSettings &settings, bool useWallet, QObject *parent)
    : QObject(parent), m_settings(settings)
{
#ifdef SLONISKO_HAVE_KEYCHAIN
    m_useWallet = useWallet && QKeychain::isAvailable();
#else
    Q_UNUSED(useWallet);
#endif
}

bool PasswordStore::walletCompiledIn()
{
#ifdef SLONISKO_HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void PasswordStore::read(const QUuid &profile, Secret secret, QObject *context,
                         std::function<void(std::optional<QString>)> done)
{
#ifdef SLONISKO_HAVE_KEYCHAIN
    if (m_useWallet) {
        auto *job = new QKeychain::ReadPasswordJob(WalletService, this);
        job->setKey(walletKey(profile, secret));
        connect(job, &QKeychain::Job::finished, context,
                [this, profile, secret, done = std::move(done)](QKeychain::Job *j) {
                    const auto *read = static_cast<QKeychain::ReadPasswordJob *>(j);
                    if (read->error() == QKeychain::NoError)
                        done(read->textData());
                    else // Not there, or the wallet failed: try the fallback.
                        done(readFallback(profile, secret));
                });
        job->start();
        return;
    }
#endif
    later(context,
          [password = readFallback(profile, secret), done = std::move(done)] { done(password); });
}

void PasswordStore::write(const QUuid &profile, Secret secret, const QString &password,
                          QObject *context, std::function<void(bool)> done)
{
#ifdef SLONISKO_HAVE_KEYCHAIN
    if (m_useWallet) {
        auto *job = new QKeychain::WritePasswordJob(WalletService, this);
        job->setKey(walletKey(profile, secret));
        job->setTextData(password);
        connect(job, &QKeychain::Job::finished, this,
                [this, profile, secret, password, context = QPointer<QObject>(context),
                 done = std::move(done)](QKeychain::Job *j) {
                    const bool inWallet = j->error() == QKeychain::NoError;
                    if (inWallet)
                        removeFallback(profile, secret);
                    else
                        writeFallback(profile, secret, password);
                    if (done && context)
                        done(inWallet);
                });
        job->start();
        return;
    }
#endif
    writeFallback(profile, secret, password);
    if (done)
        later(context, [done = std::move(done)] { done(false); });
}

void PasswordStore::remove(const QUuid &profile, Secret secret)
{
    removeFallback(profile, secret);
#ifdef SLONISKO_HAVE_KEYCHAIN
    if (m_useWallet) {
        auto *job = new QKeychain::DeletePasswordJob(WalletService, this);
        job->setKey(walletKey(profile, secret));
        job->start();
    }
#endif
}

std::optional<QString> PasswordStore::readFallback(const QUuid &profile, Secret secret) const
{
    const QVariant value = m_settings.value(fallbackKey(profile, secret));
    if (!value.isValid())
        return std::nullopt;
    return value.toString();
}

void PasswordStore::writeFallback(const QUuid &profile, Secret secret, const QString &password)
{
    m_settings.setValue(fallbackKey(profile, secret), password);
}

void PasswordStore::removeFallback(const QUuid &profile, Secret secret)
{
    m_settings.remove(fallbackKey(profile, secret));
}

} // namespace slonisko::config
