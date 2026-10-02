// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QWidget>

namespace slonisko {

// A page of the main window's work area, one per tab: a SQL editor with its
// results, a result on its own, or an object's details. The main window
// knows pages only through this interface.
class WorkspacePage : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

    // The tab's text.
    virtual QString title() const = 0;
    // The color of the connection the page works with, for the tab; invalid
    // for none.
    virtual QColor color() const { return {}; }
    // Before the page closes: it may ask to save its work. False keeps it open.
    virtual bool maybeClose() { return true; }

Q_SIGNALS:
    // The title, color or tooltip changed.
    void titleChanged();
};

} // namespace slonisko
