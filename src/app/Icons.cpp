// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Icons.h"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>

#include <algorithm>

namespace slonisko::Icons {

namespace {

QIcon themed(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

} // namespace

QIcon connection(const QString &color, bool connected)
{
    static QHash<QString, QIcon> cache;
    const QString key = color + (connected ? QLatin1Char('+') : QLatin1Char('-'));
    if (const auto it = cache.constFind(key); it != cache.cend())
        return *it;

    const QColor base
        = QColor::fromString(color).isValid() ? QColor::fromString(color) : QColor(Qt::gray);
    QIcon icon;
    for (const int size : {16, 22, 32}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal pen = size / 8.0;
        painter.setPen(QPen(base.darker(130), pen));
        painter.setBrush(connected ? base : QColor(Qt::transparent));
        const qreal margin = size / 5.0;
        painter.drawEllipse(QRectF(margin, margin, size - 2 * margin, size - 2 * margin));
        icon.addPixmap(pixmap);
    }
    cache.insert(key, icon);
    return icon;
}

QIcon object(catalog::ObjectKind kind)
{
    using K = catalog::ObjectKind;
    switch (kind) {
    case K::Database:
        return themed(QStringLiteral("server-database"), QStyle::SP_DriveHDIcon);
    case K::Schema:
        return themed(QStringLiteral("folder-documents"), QStyle::SP_DirIcon);
    case K::Table:
    case K::PartitionedTable:
    case K::ForeignTable:
        return themed(QStringLiteral("table"), QStyle::SP_FileDialogDetailedView);
    case K::View:
    case K::MaterializedView:
        return themed(QStringLiteral("view-list-details"), QStyle::SP_FileDialogContentsView);
    case K::Function:
    case K::Procedure:
    case K::Aggregate:
    case K::Trigger:
    case K::EventTrigger:
        return themed(QStringLiteral("code-function"), QStyle::SP_CommandLink);
    case K::Role:
        return themed(QStringLiteral("user-identity"), QStyle::SP_DirHomeIcon);
    case K::Tablespace:
        return themed(QStringLiteral("drive-harddisk"), QStyle::SP_DriveHDIcon);
    case K::Extension:
        return themed(QStringLiteral("plugins"), QStyle::SP_FileIcon);
    default:
        return themed(QStringLiteral("text-plain"), QStyle::SP_FileIcon);
    }
}

QIcon folder()
{
    return themed(QStringLiteral("folder"), QStyle::SP_DirIcon);
}

QIcon monitoring()
{
    return themed(QStringLiteral("utilities-system-monitor"), QStyle::SP_ComputerIcon);
}

QIcon error()
{
    return themed(QStringLiteral("dialog-error"), QStyle::SP_MessageBoxCritical);
}

QIcon transaction(bool failed)
{
    // No icon theme has one, so it is drawn.
    static QIcon cache[2];
    QIcon &icon = cache[failed];
    if (!icon.isNull())
        return icon;

    const QColor base = failed ? QColor(0xc6, 0x28, 0x28) : QColor(0xe0, 0x8a, 0x00);
    for (const int size : {16, 22, 32, 48}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal pen = std::max(1.0, size / 16.0);
        painter.setPen(QPen(base.darker(150), pen));

        // A cylinder: body, then its top, and a band halfway down.
        const qreal left = size * 0.18, right = size * 0.82, top = size * 0.12,
                    bottom = size * 0.88;
        const qreal lid = size * 0.22; // Height of the elliptic ends.
        QPainterPath body;
        body.moveTo(left, top + lid / 2);
        body.lineTo(left, bottom - lid / 2);
        body.arcTo(QRectF(left, bottom - lid, right - left, lid), 180, 180);
        body.lineTo(right, top + lid / 2);
        body.arcTo(QRectF(left, top, right - left, lid), 0, -180);
        painter.setBrush(base);
        painter.drawPath(body);
        painter.setBrush(base.lighter(140));
        painter.drawEllipse(QRectF(left, top, right - left, lid));
        painter.setBrush(Qt::NoBrush);
        const qreal middle = (top + bottom) / 2 - lid / 2;
        painter.drawArc(QRectF(left, middle, right - left, lid), 180 * 16, 180 * 16);
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace slonisko::Icons
