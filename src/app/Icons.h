// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Objects.h"

#include <QIcon>

// Icons from the desktop's icon theme, with Qt's built-in ones as fallback.
namespace slonisko::Icons {

// A dot in the profile's color; filled in when connected.
QIcon connection(const QString &color, bool connected);
QIcon object(catalog::ObjectKind kind);
QIcon folder();
QIcon monitoring();
QIcon error();
// Making a session's connections again, and ending them; wherever they are
// offered, the same pictures.
QIcon reconnect();
QIcon disconnect();
// A database with work in progress: amber while a transaction is open, red
// when it failed; greyed out (disabled), no transaction.
QIcon transaction(bool failed);

} // namespace slonisko::Icons
