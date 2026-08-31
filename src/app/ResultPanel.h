// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QTabWidget>

class QPlainTextEdit;

namespace slonisko {

class PlanView;
class ResultView;

// The bottom pane for one editor: result rows, EXPLAIN plan and messages.
class ResultPanel : public QTabWidget
{
    Q_OBJECT

public:
    explicit ResultPanel(QWidget *parent = nullptr);

    ResultView *results() const { return m_results; }
    PlanView *plan() const { return m_plan; }
    QPlainTextEdit *messages() const { return m_messages; }

    void showResults();
    void showPlan();
    void showMessages();
    // Adds a line to Messages, with the time.
    void log(const QString &text, bool error = false);

private:
    ResultView *m_results = nullptr;
    PlanView *m_plan = nullptr;
    QPlainTextEdit *m_messages = nullptr;
};

} // namespace slonisko
