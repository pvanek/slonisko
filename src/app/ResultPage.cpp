// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultPage.h"

#include "ResultView.h"
#include "Session.h"

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
        // The session's colors follow edits of its profile only on reconnect,
        // like everything else about it.
        connect(m_session, &Session::stateChanged, this, &WorkspacePage::titleChanged);
    }
    refresh();
}

QColor ResultPage::color() const
{
    return m_session ? QColor::fromString(m_session->profile().color) : QColor();
}

void ResultPage::refresh()
{
    // Without a connection, the view says so.
    m_view->run(m_session ? m_session->runner() : nullptr,
                m_session ? m_session->profile().displayName() + QStringLiteral(": ") + m_title
                          : m_title,
                m_sql);
}

} // namespace slonisko
