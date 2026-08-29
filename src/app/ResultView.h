// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/QueryRunner.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QStackedWidget;
class QTableView;
class QToolButton;

namespace slonisko {

class ResultModel;

// Shows the rows of a query, or its error.
class ResultView : public QWidget
{
    Q_OBJECT

public:
    explicit ResultView(QWidget *parent = nullptr);

    // Runs sql and shows its last result. Refresh runs it again.
    void run(pg::QueryRunner *runner, const QString &title, const QByteArray &sql);

    ResultModel *model() const { return m_model; }
    bool isRunning() const { return m_running; }

Q_SIGNALS:
    void finished();

private:
    void rerun();
    void showOutcome(const pg::QueryOutcome &outcome, qint64 elapsedMs);
    void showMessage(const QString &text, bool error);

    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_refresh = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_message = nullptr;
    ResultModel *m_model = nullptr;

    QPointer<pg::QueryRunner> m_runner;
    QByteArray m_sql;
    quint64 m_generation = 0; // Ignores results of queries run before the latest one.
    bool m_running = false;
};

} // namespace slonisko
