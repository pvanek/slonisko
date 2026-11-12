// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RecentFiles.h"

#include <QFileInfo>
#include <QSettings>

namespace slonisko {

namespace {
const QString RecentKey = QStringLiteral("files/recent");

// Absolute and clean, so the same file reached two ways is listed once.
QString normalized(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}
} // namespace

RecentFiles::RecentFiles(QSettings &settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
    for (const QString &path : m_settings.value(RecentKey).toStringList()) {
        const QString file = normalized(path);
        if (!path.isEmpty() && !m_paths.contains(file))
            m_paths << file;
    }
    if (m_paths.size() > MaxCount)
        m_paths.resize(MaxCount);
}

void RecentFiles::add(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString file = normalized(path);
    if (!m_paths.isEmpty() && m_paths.first() == file)
        return;
    m_paths.removeAll(file);
    m_paths.prepend(file);
    if (m_paths.size() > MaxCount)
        m_paths.resize(MaxCount);
    store();
}

void RecentFiles::remove(const QString &path)
{
    if (m_paths.removeAll(normalized(path)) > 0)
        store();
}

void RecentFiles::clear()
{
    if (m_paths.isEmpty())
        return;
    m_paths.clear();
    store();
}

void RecentFiles::store()
{
    m_settings.setValue(RecentKey, m_paths);
    Q_EMIT changed();
}

} // namespace slonisko
