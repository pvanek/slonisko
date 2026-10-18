// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QTextStream>

#include <optional>

namespace slonisko::docgen {

// What slonisko-docgen and slonisko-quality-report share on the command line.

QTextStream &err();

// Like psql -W: from the terminal, without echoing it.
QString askPassword();

// Text as it is, or a file's contents for @file, like curl's -d. A file
// that cannot be read is reported on err(), and gives none.
std::optional<QString> textOrFile(const QString &value);

} // namespace slonisko::docgen
