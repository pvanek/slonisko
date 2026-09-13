// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultTextView.h"

#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>

namespace slonisko {

ResultTextView::ResultTextView(QWidget *parent) : QsciScintilla(parent)
{
    setUtf8(true);
    setReadOnly(true);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setWrapMode(WrapNone);
    setMarginWidth(0, 0);
    setMarginWidth(1, 0);
    setCaretLineVisible(false);
    setToolTip(tr("Alt+drag selects a block, to copy single columns"));

    // Rectangular selection: Alt+drag with the mouse, Alt+Shift with the
    // keyboard, and several of them at once.
    SendScintilla(SCI_SETMULTIPLESELECTION, 1);
    // Alt turns the drag into a block. Setting the modifier is what the
    // documentation says to do, but some builds ignore it and take Alt only
    // through this switch, so both go in.
    SendScintilla(SCI_SETRECTANGULARSELECTIONMODIFIER, SCMOD_ALT);
    SendScintilla(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);
    SendScintilla(SCI_SETADDITIONALSELECTIONTYPING, 0);
    // A block that reaches past the end of a line copies spaces for it, so
    // the columns of pasted text still line up.
    SendScintilla(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);

    applyTheme();
}

void ResultTextView::selectBlock(qsizetype anchor, qsizetype caret)
{
    SendScintilla(SCI_SETRECTANGULARSELECTIONANCHOR, static_cast<unsigned long>(anchor));
    SendScintilla(SCI_SETRECTANGULARSELECTIONCARET, static_cast<unsigned long>(caret));
}

QString ResultTextView::selection() const
{
    const auto length = SendScintilla(SCI_GETSELTEXT, 0UL, static_cast<char *>(nullptr));
    if (length <= 0)
        return {};
    QByteArray text(int(length), Qt::Uninitialized);
    SendScintilla(SCI_GETSELTEXT, 0UL, text.data());
    // Scintilla counts the terminating zero in older versions.
    if (!text.isEmpty() && text.back() == '\0')
        text.chop(1);
    return QString::fromUtf8(text);
}

void ResultTextView::changeEvent(QEvent *event)
{
    QsciScintilla::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        applyTheme();
}

void ResultTextView::applyTheme()
{
    const QPalette colors = palette();
    setColor(colors.color(QPalette::Text));
    setPaper(colors.color(QPalette::Base));
    setSelectionBackgroundColor(colors.color(QPalette::Highlight));
    setSelectionForegroundColor(colors.color(QPalette::HighlightedText));
}

} // namespace slonisko
