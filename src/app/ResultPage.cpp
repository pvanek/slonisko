// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultPage.h"

#include "ResultModel.h"
#include "ResultView.h"
#include "Session.h"
#include "pg/QueryRunner.h"

#include <QAction>
#include <QComboBox>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace slonisko {

namespace {

const QString PidColumn = QStringLiteral("pid");

} // namespace

ResultPage::ResultPage(Session *session, const QString &title, const QByteArray &sql,
                       QWidget *parent)
    : WorkspacePage(parent), m_session(session), m_title(title), m_sql(sql),
      m_view(new ResultView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    // Nothing here is edited, so its rows can be put in any order.
    m_view->setSortable(true);

    m_autoRefresh = new QComboBox(this);
    m_autoRefresh->setToolTip(tr("Run the query again every so often, while this page is shown"));
    m_autoRefresh->addItem(tr("Auto-refresh off"), 0);
    for (const int seconds : {2, 5, 10, 30, 60})
        m_autoRefresh->addItem(tr("Every %n s", nullptr, seconds), seconds);
    m_view->addHeaderWidget(m_autoRefresh);
    connect(m_autoRefresh, &QComboBox::currentIndexChanged, this, [this] {
        const int seconds = autoRefresh();
        if (seconds > 0)
            m_timer.start(seconds * 1000);
        else
            m_timer.stop();
    });
    // A page in a tab behind others waits; it catches up when shown.
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && !m_view->isRunning())
            refresh();
    });

    m_cancel
        = new QAction(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Cancel Query"), this);
    m_cancel->setToolTip(tr("Stop what the selected sessions are running; they stay connected"));
    connect(m_cancel, &QAction::triggered, this,
            [this] { signalSelected(catalog::ProcessAction::Cancel); });
    m_terminate = new QAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                              tr("Terminate Session"), this);
    m_terminate->setToolTip(tr("End the selected sessions, rolling back their transactions"));
    connect(m_terminate, &QAction::triggered, this,
            [this] { signalSelected(catalog::ProcessAction::Terminate); });
    m_processButtons = {m_view->addHeaderAction(m_cancel), m_view->addHeaderAction(m_terminate)};
    connect(m_view->table()->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &ResultPage::updateProcessActions);
    connect(m_view->model(), &QAbstractItemModel::modelReset, this,
            &ResultPage::updateProcessActions);
    m_view->addContextAction(m_cancel);
    m_view->addContextAction(m_terminate);
    setProcessActions(false);

    if (m_session) {
        setToolTip(m_session->profile().displayName());
        connect(m_session, &Session::stateChanged, this, [this](Session::State state) {
            // Disconnected: its tunnel, if any, is gone, and so is this connection.
            if (state != Session::State::Connected && m_connection)
                m_connection->close();
            // Back after reconnecting: so is this page.
            if (state == Session::State::Connected) {
                openConnection();
                refresh();
            }
            Q_EMIT titleChanged();
        });
        if (m_session->state() == Session::State::Connected)
            openConnection();
    }
    refresh();
}

void ResultPage::openConnection()
{
    if (!m_connection) {
        m_connection = new pg::Connection(this);
        m_runner = new pg::QueryRunner(m_connection, this);
    }
    m_connection->open(m_session->conninfo());
}

QColor ResultPage::color() const
{
    return m_session ? QColor::fromString(m_session->profile().color) : QColor();
}

void ResultPage::refresh()
{
    // Without a connection, the view says so. Queries wait while it connects.
    const bool usable = m_session && m_connection
        && m_connection->state() != pg::Connection::State::Disconnected
        && m_connection->state() != pg::Connection::State::Failed;
    m_view->run(usable ? m_runner : nullptr,
                m_session ? m_session->profile().displayName() + QStringLiteral(": ") + m_title
                          : m_title,
                m_sql);
}

void ResultPage::setAutoRefresh(int seconds)
{
    const int index = m_autoRefresh->findData(seconds);
    m_autoRefresh->setCurrentIndex(index >= 0 ? index : 0);
}

int ResultPage::autoRefresh() const
{
    return m_autoRefresh->currentData().toInt();
}

void ResultPage::showEvent(QShowEvent *event)
{
    WorkspacePage::showEvent(event);
    if (autoRefresh() > 0 && !m_view->isRunning()) {
        refresh();
        m_timer.start();
    }
}

void ResultPage::setProcessActions(bool on)
{
    m_processes = on;
    // Refreshing keeps the same processes selected, by pid.
    m_view->setKeyColumn(on ? PidColumn : QString());
    for (QToolButton *button : m_processButtons)
        button->setVisible(on);
    m_cancel->setVisible(on);
    m_terminate->setVisible(on);
    updateProcessActions();
}

std::vector<int> ResultPage::selectedPids() const
{
    std::vector<int> pids;
    const int column = m_view->columnIndex(PidColumn);
    if (column < 0)
        return pids;
    const pg::RowStore &rows = m_view->model()->rows();
    for (const int row : m_view->selectedRows()) {
        if (rows.isNull(row, column))
            continue; // A lock of a prepared transaction has no process.
        const int pid = rows.value(row, column).toInt();
        if (pid > 0 && std::ranges::find(pids, pid) == pids.end())
            pids.push_back(pid);
    }
    return pids;
}

void ResultPage::updateProcessActions()
{
    const bool any = m_processes && !selectedPids().empty();
    m_cancel->setEnabled(any);
    m_terminate->setEnabled(any);
}

void ResultPage::signalSelected(catalog::ProcessAction action)
{
    const std::vector<int> pids = selectedPids();
    if (!m_processes || pids.empty() || !m_runner)
        return;
    QStringList list;
    for (const int pid : pids)
        list << QString::number(pid);
    const bool cancel = action == catalog::ProcessAction::Cancel;
    const int n = int(pids.size());
    const QString question = cancel
        ? tr("Cancel the query of %n session(s) (%1)? They stay connected.", nullptr, n)
              .arg(list.join(QStringLiteral(", ")))
        : tr("Terminate %n session(s) (%1)? Their transactions are rolled back and their "
             "clients disconnected.",
             nullptr, n)
              .arg(list.join(QStringLiteral(", ")));
    const bool confirmed = m_confirm
        ? m_confirm(question)
        : QMessageBox::question(this, cancel ? tr("Cancel Query") : tr("Terminate Session"),
                                question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
    if (!confirmed)
        return;

    m_runner->run(catalog::processActionSql(action, pids), this,
                  [this, cancel](const pg::QueryOutcome &outcome) {
                      if (!outcome.ok()) {
                          m_view->setNote(outcome.error.trimmed(), true);
                          return;
                      }
                      const pg::Result &result = outcome.results.back();
                      QStringList failed;
                      int done = 0;
                      for (int row = 0; row < result.rowCount(); ++row) {
                          if (result.value(row, 1) == "t")
                              ++done;
                          else
                              failed << QString::fromUtf8(result.value(row, 0));
                      }
                      if (failed.isEmpty())
                          m_view->setNote(cancel ? tr("Cancelled %n query(s).", nullptr, done)
                                                 : tr("Terminated %n session(s).", nullptr, done));
                      else
                          m_view->setNote(
                              tr("Could not signal %1: gone already, or not allowed for this role.")
                                  .arg(failed.join(QStringLiteral(", "))),
                              true);
                      refresh();
                  });
}

} // namespace slonisko
