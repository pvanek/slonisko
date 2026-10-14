// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Folders.h"

namespace slonisko::config {

namespace {

const QChar Separator = QLatin1Char('/');

} // namespace

QStringList folderNames(const QString &path)
{
    QStringList out;
    for (const QString &name : path.split(Separator, Qt::SkipEmptyParts)) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            out << trimmed;
    }
    return out;
}

QString normalizedFolder(const QString &path)
{
    return folderNames(path).join(Separator);
}

QString parentFolder(const QString &path)
{
    QStringList names = folderNames(path);
    if (!names.isEmpty())
        names.removeLast();
    return names.join(Separator);
}

QString folderName(const QString &path)
{
    const QStringList names = folderNames(path);
    return names.isEmpty() ? QString() : names.last();
}

QString childFolder(const QString &parent, const QString &name)
{
    return normalizedFolder(parent + Separator + name);
}

bool isWithinFolder(const QString &path, const QString &folder)
{
    const QString p = normalizedFolder(path);
    const QString f = normalizedFolder(folder);
    return f.isEmpty() || p == f || p.startsWith(f + Separator);
}

QString movedFolder(const QString &path, const QString &folder, const QString &to)
{
    const QString p = normalizedFolder(path);
    const QString f = normalizedFolder(folder);
    if (f.isEmpty() || !isWithinFolder(p, f))
        return p;
    return normalizedFolder(to + p.mid(f.size()));
}

} // namespace slonisko::config
