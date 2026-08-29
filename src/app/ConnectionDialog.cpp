// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConnectionDialog.h"

#include "Session.h"
#include "ui_ConnectionDialog.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QPainter>

namespace slonisko {

using config::PasswordMode;
using config::SshTunnelSettings;
using Secret = config::PasswordStore::Secret;

namespace {

QIcon swatch(const QString &color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(Qt::gray);
    if (QColor::fromString(color).isValid()) {
        painter.setBrush(QColor::fromString(color));
    } else {
        painter.drawLine(1, 15, 15, 1); // No color.
    }
    painter.drawRect(0, 0, 15, 15);
    return QIcon(pixmap);
}

// The items of the passwordMode and sshAuth combo boxes in the .ui file are
// in the order of these enums.
PasswordMode passwordModeAt(int index)
{
    return PasswordMode(index);
}

SshTunnelSettings::Auth authAt(int index)
{
    return SshTunnelSettings::Auth(index);
}

} // namespace

QString formatServerVersion(int version)
{
    if (version >= 100000)
        return QStringLiteral("%1.%2").arg(version / 10000).arg(version % 10000);
    return QStringLiteral("%1.%2.%3")
        .arg(version / 10000)
        .arg(version / 100 % 100)
        .arg(version % 100);
}

ConnectionDialog::ConnectionDialog(const config::ConnectionProfile &profile,
                                   config::PasswordStore *passwords, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::ConnectionDialog>()), m_passwords(passwords)
{
    m_ui->setupUi(this);
    setWindowTitle(profile.id.isNull() ? tr("New Connection") : tr("Edit Connection"));
    m_ui->walletWarning->setVisible(!m_passwords || !m_passwords->usesWallet());

    connect(m_ui->colorButton, &QPushButton::clicked, this, &ConnectionDialog::chooseColor);
    connect(m_ui->clearColorButton, &QToolButton::clicked, this, [this] { setColor({}); });
    connect(m_ui->passwordMode, &QComboBox::currentIndexChanged, this,
            &ConnectionDialog::updateEnabled);
    connect(m_ui->password, &QLineEdit::textEdited, this, [this] { m_passwordEdited = true; });
    connect(m_ui->ssh, &QGroupBox::toggled, this, &ConnectionDialog::updateEnabled);
    connect(m_ui->sshAuth, &QComboBox::currentIndexChanged, this, &ConnectionDialog::updateEnabled);
    connect(m_ui->sshSaveSecret, &QCheckBox::toggled, this, &ConnectionDialog::updateEnabled);
    connect(m_ui->sshSecret, &QLineEdit::textEdited, this, [this] { m_sshSecretEdited = true; });

    connect(m_ui->sslRootCertBrowse, &QToolButton::clicked, this,
            [this] { browseFile(m_ui->sslRootCert, tr("Root Certificate")); });
    connect(m_ui->sslCertBrowse, &QToolButton::clicked, this,
            [this] { browseFile(m_ui->sslCert, tr("Client Certificate")); });
    connect(m_ui->sslKeyBrowse, &QToolButton::clicked, this,
            [this] { browseFile(m_ui->sslKey, tr("Client Key")); });
    connect(m_ui->sshKeyFileBrowse, &QToolButton::clicked, this,
            [this] { browseFile(m_ui->sshKeyFile, tr("Private Key")); });

    connect(m_ui->testButton, &QPushButton::clicked, this, &ConnectionDialog::testConnection);
    connect(m_ui->buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::accept);
    connect(m_ui->buttons, &QDialogButtonBox::rejected, this, &ConnectionDialog::reject);

    setProfile(profile);
}

ConnectionDialog::~ConnectionDialog() = default;

void ConnectionDialog::browseFile(QLineEdit *edit, const QString &title)
{
    const QString file = QFileDialog::getOpenFileName(this, title, edit->text());
    if (!file.isEmpty())
        edit->setText(file);
}

void ConnectionDialog::setProfile(const config::ConnectionProfile &p)
{
    Ui::ConnectionDialog &ui = *m_ui;
    m_id = p.id;
    ui.name->setText(p.name);
    setColor(p.color);
    ui.host->setText(p.host);
    ui.port->setValue(p.port);
    ui.database->setText(p.database);
    ui.user->setText(p.user);
    ui.passwordMode->setCurrentIndex(int(p.passwordMode));
    ui.showAllDatabases->setChecked(p.showAllDatabases);

    ui.sslMode->setCurrentText(p.sslMode);
    ui.sslRootCert->setText(p.sslRootCert);
    ui.sslCert->setText(p.sslCert);
    ui.sslKey->setText(p.sslKey);

    ui.ssh->setChecked(p.ssh.enabled);
    ui.sshHost->setText(p.ssh.host);
    ui.sshPort->setValue(p.ssh.port);
    ui.sshUser->setText(p.ssh.user);
    ui.sshAuth->setCurrentIndex(int(p.ssh.auth));
    ui.sshKeyFile->setText(p.ssh.keyFile);
    ui.sshSaveSecret->setChecked(p.ssh.passwordMode == PasswordMode::Save);

    ui.applicationName->setText(p.applicationName);
    ui.connectTimeout->setValue(p.connectTimeout);
    ui.extraParameters->setText(p.extraParameters);

    // A saved password is kept unless the user types a new one.
    const bool existing = !p.id.isNull();
    ui.password->clear();
    ui.sshSecret->clear();
    ui.password->setPlaceholderText(existing && p.passwordMode == PasswordMode::Save
                                        ? tr("Saved (type to change)")
                                        : QString());
    ui.sshSecret->setPlaceholderText(existing && p.ssh.passwordMode == PasswordMode::Save
                                         ? tr("Saved (type to change)")
                                         : QString());
    m_passwordEdited = false;
    m_sshSecretEdited = false;
    updateEnabled();
}

config::ConnectionProfile ConnectionDialog::profile() const
{
    const Ui::ConnectionDialog &ui = *m_ui;
    config::ConnectionProfile p;
    p.id = m_id;
    p.name = ui.name->text().trimmed();
    p.color = m_color;
    p.host = ui.host->text().trimmed();
    p.port = ui.port->value();
    p.database = ui.database->text().trimmed();
    p.user = ui.user->text().trimmed();
    p.passwordMode = passwordModeAt(ui.passwordMode->currentIndex());
    p.showAllDatabases = ui.showAllDatabases->isChecked();

    p.sslMode = ui.sslMode->currentText();
    p.sslRootCert = ui.sslRootCert->text().trimmed();
    p.sslCert = ui.sslCert->text().trimmed();
    p.sslKey = ui.sslKey->text().trimmed();

    p.ssh.enabled = ui.ssh->isChecked();
    p.ssh.host = ui.sshHost->text().trimmed();
    p.ssh.port = ui.sshPort->value();
    p.ssh.user = ui.sshUser->text().trimmed();
    p.ssh.auth = authAt(ui.sshAuth->currentIndex());
    p.ssh.keyFile = ui.sshKeyFile->text().trimmed();
    // For passwords, not saving means asking; for keys, having no passphrase.
    if (ui.sshSaveSecret->isChecked())
        p.ssh.passwordMode = PasswordMode::Save;
    else
        p.ssh.passwordMode = p.ssh.auth == SshTunnelSettings::Auth::Password ? PasswordMode::Ask
                                                                             : PasswordMode::None;

    p.applicationName = ui.applicationName->text().trimmed();
    p.connectTimeout = ui.connectTimeout->value();
    p.extraParameters = ui.extraParameters->text().trimmed();
    return p;
}

std::optional<QString> ConnectionDialog::password() const
{
    return m_passwordEdited ? std::optional(m_ui->password->text()) : std::nullopt;
}

std::optional<QString> ConnectionDialog::sshSecret() const
{
    return m_sshSecretEdited ? std::optional(m_ui->sshSecret->text()) : std::nullopt;
}

void ConnectionDialog::updateEnabled()
{
    Ui::ConnectionDialog &ui = *m_ui;
    const PasswordMode mode = passwordModeAt(ui.passwordMode->currentIndex());
    ui.password->setEnabled(mode != PasswordMode::None);
    ui.password->setToolTip(mode == PasswordMode::Ask
                                ? tr("Only used by Test Connection; you are asked when connecting")
                                : QString());

    const SshTunnelSettings::Auth auth = authAt(ui.sshAuth->currentIndex());
    const bool password = auth == SshTunnelSettings::Auth::Password;
    ui.sshKeyField->setEnabled(auth == SshTunnelSettings::Auth::KeyFile);
    ui.sshSecretLabel->setText(password ? tr("SSH pass&word:") : tr("Key pass&phrase:"));
    ui.sshSaveSecret->setText(password ? tr("Save the password")
                                       : tr("The key has a passphrase; save it"));
    ui.sshSecretLabel->setEnabled(auth != SshTunnelSettings::Auth::Agent);
    ui.sshSaveSecret->setEnabled(auth != SshTunnelSettings::Auth::Agent);
    ui.sshSecret->setEnabled(
        password || (auth == SshTunnelSettings::Auth::KeyFile && ui.sshSaveSecret->isChecked()));
}

void ConnectionDialog::setColor(const QString &color)
{
    m_color = QColor::fromString(color).isValid() ? QColor::fromString(color).name() : QString();
    m_ui->colorButton->setIcon(swatch(m_color));
}

void ConnectionDialog::chooseColor()
{
    const QColor color = QColorDialog::getColor(
        QColor::fromString(m_color).isValid() ? QColor::fromString(m_color) : QColor(Qt::white),
        this, tr("Connection Color"));
    if (color.isValid())
        setColor(color.name());
}

void ConnectionDialog::accept()
{
    const config::ConnectionProfile p = profile();
    QString problem;
    if (p.host.isEmpty())
        problem = tr("Enter the server's host name or address.");
    else if (p.ssh.enabled && p.ssh.host.isEmpty())
        problem = tr("Enter the SSH host, or turn the SSH tunnel off.");
    else if (p.ssh.enabled && p.ssh.auth == SshTunnelSettings::Auth::KeyFile
             && p.ssh.keyFile.isEmpty())
        problem = tr("Choose the SSH private key file.");
    if (!problem.isEmpty()) {
        finishTest(false, problem);
        return;
    }
    QDialog::accept();
}

void ConnectionDialog::testConnection()
{
    const config::ConnectionProfile p = profile();
    m_ui->testButton->setEnabled(false);
    m_ui->status->setStyleSheet(QString());
    m_ui->status->setText(tr("Connecting…"));

    // Typed secrets win; otherwise use the saved ones, if any.
    auto password = std::make_shared<QString>(m_ui->password->text());
    auto sshSecret = std::make_shared<QString>(m_ui->sshSecret->text());
    const bool savedPassword = !m_passwordEdited && !m_id.isNull()
        && p.passwordMode == PasswordMode::Save && m_passwords;
    const bool savedSsh = !m_sshSecretEdited && !m_id.isNull() && p.ssh.enabled
        && p.ssh.passwordMode == PasswordMode::Save && m_passwords;

    auto withSsh = [this, password, sshSecret, savedSsh] {
        if (!savedSsh) {
            startTest(*password, *sshSecret);
            return;
        }
        m_passwords->read(m_id, Secret::Ssh, this, [this, password](std::optional<QString> secret) {
            startTest(*password, secret.value_or(QString()));
        });
    };
    if (savedPassword) {
        m_passwords->read(m_id, Secret::Postgres, this,
                          [password, withSsh](std::optional<QString> saved) {
                              *password = saved.value_or(QString());
                              withSsh();
                          });
    } else {
        if (p.passwordMode == PasswordMode::None)
            password->clear();
        withSsh();
    }
}

void ConnectionDialog::startTest(const QString &password, const QString &sshSecret)
{
    delete m_test;
    m_test = new Session(profile(), {password, sshSecret}, this);
    connect(m_test, &Session::stateChanged, this, [this](Session::State state) {
        if (state == Session::State::Connected)
            finishTest(true,
                       tr("Connected to PostgreSQL %1.")
                           .arg(formatServerVersion(m_test->serverVersion())));
        else if (state == Session::State::Failed)
            finishTest(false, m_test->errorMessage());
    });
    m_test->open();
}

void ConnectionDialog::finishTest(bool ok, const QString &message)
{
    m_ui->testButton->setEnabled(true);
    m_ui->status->setStyleSheet(ok ? QStringLiteral("color: #2e7d32;")
                                   : QStringLiteral("color: #c62828;"));
    m_ui->status->setText(message);
    if (m_test) {
        m_test->deleteLater();
        m_test = nullptr;
    }
}

QString ConnectionDialog::testStatus() const
{
    return m_ui->status->text();
}

} // namespace slonisko
