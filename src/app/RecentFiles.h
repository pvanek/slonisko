// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>

class QSettings;

namespace slonisko {

// The files opened or saved last, most recent first, kept in the settings.
class RecentFiles : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxCount = 10;

    explicit RecentFiles(QSettings &settings, QObject *parent = nullptr);

    QStringList paths() const { return m_paths; }
    // Moves a file to the top, adding it if it is not there.
    void add(const QString &path);
    void remove(const QString &path);
    void clear();

Q_SIGNALS:
    void changed();

private:
    void store();

    QSettings &m_settings;
    QStringList m_paths;
};

} // namespace slonisko
