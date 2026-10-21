// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ToolDialog.h"

#include <memory>

namespace Ui {
class RestoreDialog;
}

namespace slonisko {

// Restores a backup into a database: an archive with pg_restore, a plain
// SQL script with psql.
class RestoreDialog : public ToolDialog
{
    Q_OBJECT

public:
    RestoreDialog(Session *session, const QString &database, QSettings *settings,
                  QWidget *parent = nullptr);
    ~RestoreDialog() override;

    void setPath(const QString &path);
    pg::RestoreOptions options() const;
    int nextId() const override;

private:
    QString programName() const override;
    QStringList arguments(const QByteArray &conninfo) const override;
    QString problem() const override;
    QString question() const override;
    QString doneMessage() const override;
    void pathChanged();
    bool isScript() const;

    std::unique_ptr<Ui::RestoreDialog> m_ui; // Layout in RestoreDialog.ui.
};

} // namespace slonisko
