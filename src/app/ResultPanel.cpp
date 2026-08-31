// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultPanel.h"

#include "PlanView.h"
#include "ResultView.h"

#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QTime>

namespace slonisko {

ResultPanel::ResultPanel(QWidget *parent)
    : QTabWidget(parent), m_results(new ResultView(this)), m_plan(new PlanView(this)),
      m_messages(new QPlainTextEdit(this))
{
    setDocumentMode(true);
    setTabPosition(QTabWidget::South);
    m_messages->setReadOnly(true);
    m_messages->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_messages->setMaximumBlockCount(5000);
    addTab(m_results, tr("Results"));
    addTab(m_plan, tr("Plan"));
    addTab(m_messages, tr("Messages"));
}

void ResultPanel::showResults()
{
    setCurrentWidget(m_results);
}

void ResultPanel::showPlan()
{
    setCurrentWidget(m_plan);
}

void ResultPanel::showMessages()
{
    setCurrentWidget(m_messages);
}

void ResultPanel::log(const QString &text, bool error)
{
    const QString line
        = QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text;
    if (error)
        m_messages->appendHtml(
            QStringLiteral("<span style=\"color:#c62828\">%1</span>")
                .arg(line.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    else
        m_messages->appendPlainText(line);
}

} // namespace slonisko
