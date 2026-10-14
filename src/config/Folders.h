// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

// Folders of connection profiles: paths like "stage/env1", with "/" between
// the names and the empty path for the top level.
namespace slonisko::config {

// Trims each name and drops empty ones: " a//b " becomes "a/b".
QString normalizedFolder(const QString &path);
// The names along the path; none for the top level.
QStringList folderNames(const QString &path);
// The folder holding path, empty for the top level.
QString parentFolder(const QString &path);
// The last name of the path.
QString folderName(const QString &path);
// parent/name, or name at the top level.
QString childFolder(const QString &parent, const QString &name);
// Whether path is folder or lies anywhere below it. Everything is within the
// top level.
bool isWithinFolder(const QString &path, const QString &folder);
// Where path ends up when folder moves to to: unchanged unless it is within
// folder.
QString movedFolder(const QString &path, const QString &folder, const QString &to);

} // namespace slonisko::config
