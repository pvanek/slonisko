// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SavedEditors.h"

#include <QSettings>

namespace slonisko {

namespace {
const QString EditorsKey = QStringLiteral("session/editors");
const QString CurrentKey = QStringLiteral("session/currentEditor");
const QString FileKey = QStringLiteral("file");
const QString TextKey = QStringLiteral("text");
const QString LineKey = QStringLiteral("line");
const QString ColumnKey = QStringLiteral("column");
} // namespace

SavedEditors SavedEditors::load(QSettings &settings)
{
    SavedEditors out;
    const int count = settings.beginReadArray(EditorsKey);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SavedEditor editor;
        editor.filePath = settings.value(FileKey).toString();
        if (editor.filePath.isEmpty())
            editor.text = settings.value(TextKey).toString();
        editor.line = settings.value(LineKey, 0).toInt();
        editor.column = settings.value(ColumnKey, 0).toInt();
        out.editors << editor;
    }
    settings.endArray();
    out.current = settings.value(CurrentKey, -1).toInt();
    if (out.current >= out.editors.size())
        out.current = -1;
    return out;
}

void SavedEditors::store(QSettings &settings) const
{
    // Removed first: a shorter list would leave the old entries behind it.
    settings.remove(EditorsKey);
    settings.beginWriteArray(EditorsKey, int(editors.size()));
    for (int i = 0; i < editors.size(); ++i) {
        const SavedEditor &editor = editors[i];
        settings.setArrayIndex(i);
        if (editor.filePath.isEmpty())
            settings.setValue(TextKey, editor.text);
        else
            settings.setValue(FileKey, editor.filePath);
        settings.setValue(LineKey, editor.line);
        settings.setValue(ColumnKey, editor.column);
    }
    settings.endArray();
    settings.setValue(CurrentKey, current);
}

} // namespace slonisko
