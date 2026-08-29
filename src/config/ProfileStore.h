// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/ConnectionProfile.h"

#include <vector>

class QSettings;

namespace slonisko::config {

// Connection profiles in QSettings, one group per profile under
// "connections/<id>". Passwords are not stored here; see PasswordStore.
class ProfileStore
{
public:
    explicit ProfileStore(QSettings &settings) : m_settings(settings) { }

    // Sorted by display name, case-insensitively.
    std::vector<ConnectionProfile> load() const;
    // Assigns a new id first if the profile has none.
    void save(ConnectionProfile &profile);
    void remove(const QUuid &id);

private:
    QSettings &m_settings;
};

} // namespace slonisko::config
