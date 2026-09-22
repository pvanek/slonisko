// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "WorkspacePage.h"
#include "catalog/Details.h"

#include <QPointer>

class QLabel;
class QTabWidget;

namespace slonisko {

class ResultTextView;
class Session;

// What one database object is: its properties, the lists that belong to it
// (columns, indexes, …) and its definition, each on a tab of its own.
class ObjectPage : public WorkspacePage
{
    Q_OBJECT

public:
    ObjectPage(Session *session, const QString &database, catalog::ObjectKind kind,
               catalog::Oid oid, const QString &name, QWidget *parent = nullptr);

    QString title() const override { return m_name; }
    QColor color() const override;

    Session *session() const { return m_session; }
    catalog::Oid oid() const { return m_oid; }
    catalog::ObjectKind kind() const { return m_kind; }
    const catalog::ObjectDetail &detail() const { return m_detail; }
    QTabWidget *tabs() const { return m_tabs; }
    // Reads everything about the object again.
    void refresh();

private:
    void showDetail(const catalog::ObjectDetail &detail);
    void showMessage(const QString &text, bool error = false);

    QPointer<Session> m_session;
    QString m_database;
    catalog::ObjectKind m_kind;
    catalog::Oid m_oid = 0;
    QString m_name;
    catalog::ObjectDetail m_detail;

    QLabel *m_heading = nullptr;
    QLabel *m_message = nullptr;
    QTabWidget *m_tabs = nullptr;
    ResultTextView *m_definition = nullptr;
    quint64 m_generation = 0; // Drops the answers of an earlier refresh.
};

} // namespace slonisko
