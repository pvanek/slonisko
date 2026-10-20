// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/ValueFormat.h"

#include <QDialog>

#include <memory>

namespace Ui {
class ValueViewer;
}

namespace slonisko {

class ResultTextView;
class ValueLexer;

// One value of a result in a window of its own, read-only: a JSON document,
// an array or a long text, formatted and highlighted as what it is. The
// value is a copy, so the window stays as it was when the query runs again.
class ValueViewer : public QDialog
{
    Q_OBJECT

public:
    // title says where the value is from, such as its column and row.
    ValueViewer(const QString &title, Oid type, const QByteArray &value, QWidget *parent = nullptr);
    ~ValueViewer() override;

    catalog::ValueSyntax syntax() const;
    void setSyntax(catalog::ValueSyntax syntax);
    bool isFormatted() const;
    void setFormatted(bool formatted);
    bool wrapsLines() const;
    void setWrapLines(bool wrap);
    // What is on show, formatted or not.
    QString text() const;
    // The size of the value, or why it could not be formatted.
    QString status() const;
    ResultTextView *editor() const;

protected:
    void changeEvent(QEvent *event) override;

private:
    void showValue();

    std::unique_ptr<Ui::ValueViewer> m_ui; // Layout in ValueViewer.ui.
    ValueLexer *m_lexer = nullptr;
    QByteArray m_value;
};

} // namespace slonisko
