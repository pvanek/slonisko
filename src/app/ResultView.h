// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pg/QueryRunner.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QAction;
class QLabel;
class QStackedWidget;
class QTableView;
class QToolButton;

namespace slonisko {

class ResultModel;

// Shows the rows of a query, or its error. Either runs the query itself
// (run()), or is fed by whoever runs it (begin(), append(), finish()).
class ResultView : public QWidget
{
    Q_OBJECT

public:
    explicit ResultView(QWidget *parent = nullptr);

    // Runs sql and shows its last result. Refresh runs it again.
    void run(pg::QueryRunner *runner, const QString &title, const QByteArray &sql);

    void begin(const QString &title);
    void append(const pg::Result &result);
    void finish(const QString &status);
    void showError(const QString &message);
    void showMessage(const QString &text);
    // What the Run Again button does; none disables it.
    void setRerun(std::function<void()> rerun);

    ResultModel *model() const { return m_model; }
    QTableView *table() const { return m_table; }
    bool isRunning() const { return m_running; }
    // Cancels the running query, if it is one this view runs itself.
    void stop();

Q_SIGNALS:
    void finished();
    // Save was asked for; whoever ran the query saves the model's changes.
    void saveRequested();

private:
    void rerunQuery();
    void showOutcome(const pg::QueryOutcome &outcome, qint64 elapsedMs);
    void setMessage(const QString &text, bool error);
    void updateEditing();
    void refresh();

    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_refresh = nullptr;
    QToolButton *m_stop = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_message = nullptr;
    ResultModel *m_model = nullptr;
    QLabel *m_editLabel = nullptr;
    QWidget *m_editBar = nullptr;
    QAction *m_addRow = nullptr;
    QAction *m_deleteRows = nullptr;
    QAction *m_setNull = nullptr;
    QAction *m_save = nullptr;
    QAction *m_discard = nullptr;
    QAction *m_rerunAction = nullptr;

    std::function<void()> m_rerun;
    QPointer<pg::QueryRunner> m_runner;
    QByteArray m_sql;
    quint64 m_generation = 0; // Ignores results of queries run before the latest one.
    bool m_running = false;
    bool m_sized = false; // Columns sized to their contents once rows arrived.
};

} // namespace slonisko
