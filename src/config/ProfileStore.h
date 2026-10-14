// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/ConnectionProfile.h"

#include <QStringList>

#include <vector>

class QSettings;

namespace slonisko::config {

// Connection profiles in QSettings, one group per profile under
// "connections/<id>", and the folders they are sorted into. Passwords are
// not stored here; see PasswordStore.
class ProfileStore
{
public:
    explicit ProfileStore(QSettings &settings) : m_settings(settings) { }

    // Sorted by display name, case-insensitively.
    std::vector<ConnectionProfile> load() const;
    // Assigns a new id first if the profile has none, and adds its folder.
    void save(ConnectionProfile &profile);
    void remove(const QUuid &id);

    // Every folder, empty or not, with the folders above it; unsorted.
    QStringList folders() const;
    void addFolder(const QString &path);
    // Moves folder, with everything in it, to the path to; merges it into
    // to if that exists already.
    void moveFolder(const QString &folder, const QString &to);
    // Removes folder; what was in it moves up a level.
    void removeFolder(const QString &folder);

    // The order the user put connections and folders in, as the browser
    // describes it.
    QStringList order() const;
    void setOrder(const QStringList &order);

private:
    void setFolders(const QStringList &folders);

    QSettings &m_settings;
};

} // namespace slonisko::config
