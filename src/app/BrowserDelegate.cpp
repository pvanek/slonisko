// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserDelegate.h"

#include "BrowserModel.h"

#include <QApplication>
#include <QPainter>

namespace slonisko {

namespace {

constexpr int Gap = 8; // Between the name and the detail, in pixels.

} // namespace

void BrowserDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    const QString detail = index.data(BrowserModel::DetailRole).toString();
    if (detail.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    // The background, selection and icon, then both texts by hand.
    const QString text = opt.text;
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QRect rect
        = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -2, 0);
    const bool selected = opt.state & QStyle::State_Selected;
    const QPalette::ColorGroup group
        = opt.state & QStyle::State_Enabled ? QPalette::Normal : QPalette::Disabled;

    painter->save();
    painter->setFont(opt.font);
    const QColor textColor = selected ? opt.palette.color(group, QPalette::HighlightedText)
                                      : opt.palette.color(group, QPalette::Text);
    const QVariant foreground = index.data(Qt::ForegroundRole);
    painter->setPen(foreground.canConvert<QColor>() && !selected ? foreground.value<QColor>()
                                                                 : textColor);
    const QString name = opt.fontMetrics.elidedText(text, Qt::ElideRight, rect.width());
    painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, name);

    const int nameWidth = opt.fontMetrics.horizontalAdvance(name) + Gap;
    if (nameWidth < rect.width()) {
        QRect detailRect = rect.adjusted(nameWidth, 0, 0, 0);
        QColor dim = textColor;
        dim.setAlphaF(0.55f);
        painter->setPen(dim);
        painter->drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                          opt.fontMetrics.elidedText(detail, Qt::ElideRight, detailRect.width()));
    }
    painter->restore();
}

QSize BrowserDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const QString detail = index.data(BrowserModel::DetailRole).toString();
    if (!detail.isEmpty())
        size.rwidth() += Gap + option.fontMetrics.horizontalAdvance(detail);
    return size;
}

} // namespace slonisko
