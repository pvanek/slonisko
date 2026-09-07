// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultPage.h"

#include "ResultView.h"
#include "Session.h"
#include "pg/QueryRunner.h"

#include <QVBoxLayout>

namespace slonisko {

ResultPage::ResultPage(Session *session, const QString &title, const QByteArray &sql,
                       QWidget *parent)
    : WorkspacePage(parent), m_session(session), m_title(title), m_sql(sql),
      m_view(new ResultView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    if (m_session) {
        setToolTip(m_session->profile().displayName());
        connect(m_session, &Session::stateChanged, this, [this](Session::State state) {
            // Disconnected: its tunnel, if any, is gone, and so is this connection.
            if (state != Session::State::Connected && m_connection)
                m_connection->close();
            Q_EMIT titleChanged();
        });
        if (m_session->state() == Session::State::Connected) {
            m_connection = new pg::Connection(this);
            m_runner = new pg::QueryRunner(m_connection, this);
            m_connection->open(m_session->conninfo());
        }
    }
    refresh();
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

} // namespace slonisko
