// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultView.h"

#include "ResultModel.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace slonisko {

ResultView::ResultView(QWidget *parent)
    : QWidget(parent), m_title(new QLabel(this)), m_status(new QLabel(this)),
      m_refresh(new QToolButton(this)), m_stack(new QStackedWidget(this)),
      m_table(new QTableView(this)), m_message(new QLabel(this)), m_model(new ResultModel(this))
{
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_refresh->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_refresh->setToolTip(tr("Run again"));
    m_refresh->setAutoRaise(true);
    m_refresh->setEnabled(false);
    connect(m_refresh, &QToolButton::clicked, this, &ResultView::rerun);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(4, 2, 4, 2);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_status);
    header->addWidget(m_refresh);

    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);

    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setMargin(8);

    m_stack->addWidget(m_table);
    m_stack->addWidget(m_message);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack);

    showMessage(tr("Double-click an item under DBA Tools or System Info to see it here."), false);
}

void ResultView::run(pg::QueryRunner *runner, const QString &title, const QByteArray &sql)
{
    m_runner = runner;
    m_sql = sql;
    m_title->setText(title);
    rerun();
}

void ResultView::rerun()
{
    if (!m_runner) {
        showMessage(tr("Not connected"), true);
        return;
    }
    const quint64 generation = ++m_generation;
    m_running = true;
    m_refresh->setEnabled(false);
    m_status->setText(tr("Running…"));

    QElapsedTimer timer;
    timer.start();
    m_runner->run(m_sql, this, [this, generation, timer](const pg::QueryOutcome &outcome) {
        if (generation == m_generation)
            showOutcome(outcome, timer.elapsed());
    });
}

void ResultView::showOutcome(const pg::QueryOutcome &outcome, qint64 elapsedMs)
{
    m_running = false;
    m_refresh->setEnabled(true);
    const QString time = tr("%1 ms").arg(elapsedMs);

    if (!outcome.ok()) {
        m_model->clear();
        m_status->setText(time);
        showMessage(outcome.error, true);
    } else {
        const pg::Result &last = outcome.results.back();
        m_model->setResult(last);
        m_status->setText(tr("%n row(s), %1", nullptr, last.rowCount()).arg(time));
        m_stack->setCurrentWidget(m_table);
        m_table->resizeColumnsToContents();
        // Keep very wide columns, like query texts, from taking all the space.
        for (int c = 0; c < m_model->columnCount(); ++c)
            m_table->setColumnWidth(c, std::min(m_table->columnWidth(c), 400));
    }
    Q_EMIT finished();
}

void ResultView::showMessage(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
    m_stack->setCurrentWidget(m_message);
}

} // namespace slonisko
