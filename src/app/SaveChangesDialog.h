// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayList>
#include <QDialog>

#include <memory>

namespace Ui {
class SaveChangesDialog;
}

namespace slonisko {

// Shows the statements that save a result's edits, before they run.
class SaveChangesDialog : public QDialog
{
    Q_OBJECT

public:
    SaveChangesDialog(const QString &table, const QByteArrayList &statements, bool inTransaction,
                      QWidget *parent = nullptr);
    ~SaveChangesDialog() override;

private:
    std::unique_ptr<Ui::SaveChangesDialog> m_ui; // Layout in SaveChangesDialog.ui.
};

} // namespace slonisko
