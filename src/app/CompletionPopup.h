// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Completion.h"

#include <QFrame>

class QKeyEvent;
class QListView;

namespace slonisko {

class CompletionModel;

// The list of completion suggestions under the cursor. It never takes the
// keyboard focus: the editor keeps it and forwards navigation keys here.
class CompletionPopup : public QFrame
{
    Q_OBJECT

public:
    explicit CompletionPopup(QWidget *editor);

    void showItems(std::vector<catalog::CompletionItem> items, const QPoint &globalPos);
    // Handles Up, Down, Page Up/Down, Enter, Tab and Escape while visible.
    bool handleKey(QKeyEvent *event);
    const catalog::CompletionItem *current() const;
    int count() const;

Q_SIGNALS:
    void accepted(const catalog::CompletionItem &item);

private:
    void move(int rows);
    void accept();

    QListView *m_list = nullptr;
    CompletionModel *m_model = nullptr;
};

} // namespace slonisko
