// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

namespace slonisko {

class PageTabWidget;

// A window of its own for pages dragged out of the main window. It closes
// when its last page leaves; closing it closes its pages.
class PageWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PageWindow(QWidget *parent = nullptr);

    PageTabWidget *tabs() const { return m_tabs; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void updateTitle();

    PageTabWidget *m_tabs;
};

} // namespace slonisko
