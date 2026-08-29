// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Icons.h"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

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

} // namespace slonisko::Icons
