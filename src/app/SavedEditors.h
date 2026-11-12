// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QString>

class QSettings;

namespace slonisko {

// The editors open when the application quit, to open again on its start.
struct SavedEditor
{
    QString filePath; // Empty for an editor without a file; then the text is kept.
    QString text;
    int line = 0;
    int column = 0;

    bool operator==(const SavedEditor &) const = default;
};

struct SavedEditors
{
    QList<SavedEditor> editors;
    int current = -1; // Index into editors, or -1.

    static SavedEditors load(QSettings &settings);
    void store(QSettings &settings) const;
};

} // namespace slonisko
