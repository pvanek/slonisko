// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ToolDialog.h"

#include <memory>

namespace Ui {
class DumpDialog;
}

namespace slonisko {

// Backs up a database, or some of its schemas or tables, with pg_dump.
class DumpDialog : public ToolDialog
{
    Q_OBJECT

public:
    // schema and table, when set, are what is backed up; otherwise all of it.
    DumpDialog(Session *session, const QString &database, const QString &schema,
               const QString &table, QSettings *settings, QWidget *parent = nullptr);
    ~DumpDialog() override;

    pg::DumpOptions options() const;

private:
    QString programName() const override;
    QStringList arguments(const QByteArray &conninfo) const override;
    QString problem() const override;
    QString question() const override;
    QString doneMessage() const override;
    void formatChanged();
    void browse();

    std::unique_ptr<Ui::DumpDialog> m_ui; // Layout in DumpDialog.ui.
};

} // namespace slonisko
