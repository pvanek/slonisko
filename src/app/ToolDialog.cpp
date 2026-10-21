// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ToolDialog.h"

#include "Session.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>

namespace slonisko {

void ToolPage::setComplete(bool complete)
{
    if (complete == m_complete)
        return;
    m_complete = complete;
    Q_EMIT completeChanged();
}

ToolDialog::ToolDialog(Session *session, const QString &database, QSettings *settings,
                       QWidget *parent)
    : QWizard(parent), m_session(session), m_database(database), m_settings(settings)
{
    m_confirm = [this](const QString &question) {
        return QMessageBox::question(this, windowTitle(), question) == QMessageBox::Yes;
    };
}

ToolDialog::~ToolDialog()
{
    if (m_run)
        m_run->disconnect(this);
}

void ToolDialog::setWidgets(const Widgets &widgets, const QString &runText)
{
    m_widgets = widgets;
    setOption(QWizard::NoBackButtonOnStartPage);
    setButtonText(QWizard::FinishButton, runText);
    setButtonText(QWizard::CancelButton, tr("&Close"));
    m_widgets.busy->hide();
    connect(m_widgets.chooseTool, &QPushButton::clicked, this, &ToolDialog::chooseTool);
    // Back on the last page, it can run again with what was changed.
    connect(this, &QWizard::currentIdChanged, this, [this] {
        m_done = false;
        m_widgets.status->clear();
        update();
    });
}

bool ToolDialog::isRunning() const
{
    return m_run && m_run->isRunning();
}

QString ToolDialog::commandText() const
{
    return m_widgets.command->text();
}

QString ToolDialog::logText() const
{
    return m_widgets.log->toPlainText();
}

QString ToolDialog::problemText() const
{
    return m_widgets.problem->text();
}

QString ToolDialog::statusText() const
{
    return m_widgets.status->text();
}

bool ToolDialog::canRun() const
{
    return m_widgets.filePage->isComplete() && m_widgets.runPage->isComplete();
}

QStringList ToolDialog::nameList(const QString &list)
{
    QStringList out;
    for (const QString &name : list.split(QLatin1Char(','))) {
        if (const QString trimmed = name.trimmed(); !trimmed.isEmpty())
            out << trimmed;
    }
    return out;
}

void ToolDialog::fillContents(QComboBox *contents)
{
    contents->addItem(tr("Schema and data"), int(pg::DumpContent::All));
    contents->addItem(tr("Schema only"), int(pg::DumpContent::SchemaOnly));
    contents->addItem(tr("Data only"), int(pg::DumpContent::DataOnly));
}

QString ToolDialog::databaseName() const
{
    if (!m_database.isEmpty() || !m_session)
        return m_database;
    return m_session->profile().database;
}

std::optional<pg::ClientTool> ToolDialog::tool(const QString &name)
{
    if (const auto found = m_tools.find(name); found != m_tools.end())
        return found->second;
    QStringList dirs;
    if (m_settings) {
        if (const QString dir = m_settings->value(QLatin1String(DirectoryKey)).toString();
            !dir.isEmpty())
            dirs << dir;
    }
    const int server = m_session ? m_session->serverVersion() : 0;
    const std::vector<pg::ClientTool> tools = pg::findClientTools(name, dirs);
    // One chosen by hand is used whatever its version.
    std::optional<pg::ClientTool> chosen;
    if (!dirs.isEmpty() && !tools.empty()
        && QFileInfo(tools.front().path).absolutePath() == QDir(dirs.first()).absolutePath())
        chosen = tools.front();
    else
        chosen = pg::chooseClientTool(tools, server);
    m_tools[name] = chosen;
    return chosen;
}

void ToolDialog::chooseTool()
{
    const std::optional<pg::ClientTool> current = tool(programName());
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Where Is %1?").arg(programName()),
        current ? QFileInfo(current->path).absolutePath() : QString());
    if (path.isEmpty())
        return;
    if (m_settings)
        m_settings->setValue(QLatin1String(DirectoryKey), QFileInfo(path).absolutePath());
    m_tools.clear();
    update();
}

void ToolDialog::update()
{
    if (isRunning())
        return;
    const QString name = programName();
    const std::optional<pg::ClientTool> found = tool(name);
    const int server = m_session ? m_session->serverVersion() : 0;

    QString problem;
    if (!m_session || m_session->state() != Session::State::Connected)
        problem = tr("Not connected.");
    else if (!found)
        problem = tr("%1 was not found. Install PostgreSQL's client programs, or choose where "
                     "%1 is.")
                      .arg(name);
    else
        problem = this->problem();

    if (found) {
        m_widgets.tool->setText(
            tr("%1 %2, %3")
                .arg(name, pg::versionText(found->version), QDir::toNativeSeparators(found->path)));
        m_widgets.command->setText(pg::commandLine(
            found->path,
            arguments(m_session ? m_session->toolConninfo(m_database) : QByteArray())));
    } else {
        m_widgets.tool->setText(tr("%1 not found").arg(name));
        m_widgets.command->clear();
    }
    // Only a warning: an older one may still do, as for a plain script.
    QString warning;
    if (found && name != QLatin1String("psql") && found->version / 10000 < server / 10000) {
        warning = tr("%1 %2 is older than the server (%3) and may refuse to work with it.")
                      .arg(name, pg::versionText(found->version), pg::versionText(server));
    }
    m_widgets.filePage->setComplete(problem.isEmpty());
    m_widgets.runPage->setComplete(problem.isEmpty() && !m_done);
    setText(m_widgets.problem, problem.isEmpty() ? warning : problem,
            !problem.isEmpty() || !warning.isEmpty());
}

void ToolDialog::run()
{
    update();
    if (isRunning() || !canRun())
        return;
    const std::optional<pg::ClientTool> found = tool(programName());
    if (!found)
        return;
    if (const QString ask = question(); !ask.isEmpty() && !m_confirm(ask))
        return;

    const QStringList args = arguments(m_session->toolConninfo(m_database));
    m_widgets.log->clear();
    m_widgets.log->appendPlainText(pg::commandLine(found->path, args));
    m_run = new pg::ClientToolRun(this);
    connect(m_run, &pg::ClientToolRun::output, m_widgets.log, &QPlainTextEdit::appendPlainText);
    connect(m_run, &pg::ClientToolRun::finished, this, &ToolDialog::onFinished);
    m_run->start(found->path, args, m_session->password());

    // Nothing to change while it runs: the choices are on the pages before.
    setOption(QWizard::DisabledBackButtonOnLastPage);
    m_widgets.runPage->setComplete(false);
    setButtonText(QWizard::CancelButton, tr("&Stop"));
    m_widgets.busy->show();
    setText(m_widgets.status, tr("Running %1…").arg(programName()), false);
}

void ToolDialog::onFinished(bool ok, const QString &message)
{
    m_run->deleteLater();
    m_run = nullptr;
    setOption(QWizard::DisabledBackButtonOnLastPage, false);
    setButtonText(QWizard::CancelButton, tr("&Close"));
    m_widgets.busy->hide();
    // Done is done: running it again takes going back to change something.
    m_done = ok;
    update();
    setText(m_widgets.status, ok ? doneMessage() : message, !ok);
    Q_EMIT toolFinished(ok);
}

void ToolDialog::setText(QLabel *label, const QString &text, bool error)
{
    label->setText(text);
    // A red that reads on a dark background too.
    const bool dark = label->palette().color(QPalette::Window).lightness() < 128;
    label->setStyleSheet(
        error ? QStringLiteral("color: %1;")
                    .arg(dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c62828"))
              : QString());
}

void ToolDialog::accept()
{
    run();
}

void ToolDialog::reject()
{
    if (isRunning()) {
        m_run->cancel();
        return;
    }
    QWizard::reject();
}

} // namespace slonisko
