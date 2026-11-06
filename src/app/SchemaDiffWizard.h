// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "schemadiff/Diff.h"

#include <QPointer>
#include <QUuid>
#include <QWizard>

#include <memory>
#include <optional>

class QComboBox;
class QLabel;

namespace Ui {
class SchemaDiffWizard;
}

namespace slonisko {

class ConnectionBrowser;
class Session;

// Tools → Compare Schemas: two of the browser's connections, each to a
// database, the schemas to compare, and then what differs and the script
// that makes Target like Reference. Connections not open yet are opened, as
// the browser would.
class SchemaDiffWizard : public QWizard
{
    Q_OBJECT

public:
    enum PageId { ConnectionsPage, SchemasPage, ResultPage };

    explicit SchemaDiffWizard(ConnectionBrowser *browser, QWidget *parent = nullptr);
    ~SchemaDiffWizard() override;

    // What is chosen, for tests to set without the combo boxes.
    void setTarget(const QUuid &profile, const QString &database = {});
    void setReference(const QUuid &profile, const QString &database = {});
    QUuid targetProfile() const;
    QUuid referenceProfile() const;
    QString targetDatabase() const;
    QString referenceDatabase() const;

    Session *targetSession() const;
    Session *referenceSession() const;
    bool isComparing() const { return m_comparing; }
    const schemadiff::Diff &diff() const { return m_diff; }
    QString script() const { return m_script; }

    bool validateCurrentPage() override;

Q_SIGNALS:
    // The comparison is done, or failed.
    void compared(bool ok);
    // Open in SQL Editor: the script, for an editor on session's database.
    void scriptRequested(slonisko::Session *session, const QString &database,
                         const QString &script);

protected:
    void initializePage(int id) override;

private:
    struct Side
    {
        QComboBox *profile;
        QComboBox *database;
        QLabel *state;
    };

    void fillProfiles();
    void choose(const Side &side, const QUuid &profile, const QString &database);
    void profileChanged(const Side &side);
    void updateConnections();
    void loadDatabases(const Side &side);
    void loadSchemas();
    void compare();
    void showDiff();
    void updateScript();
    void showChange();
    void fail(const QString &message);
    void saveScript();
    void saveSettings() const;
    Session *session(const Side &side) const;
    QString databaseOf(const Side &side) const;
    QString sideName(const Side &side) const;

    std::unique_ptr<Ui::SchemaDiffWizard> m_ui; // Layout in SchemaDiffWizard.ui.
    QPointer<ConnectionBrowser> m_browser;
    Side m_target;
    Side m_reference;
    // Next was pressed while connections were still being opened.
    bool m_advanceWhenConnected = false;
    bool m_comparing = false;
    bool m_succeeded = false;
    // Bumped by every comparison, so that one started earlier and finishing
    // late is not taken for the current one.
    int m_generation = 0;
    std::optional<docgen::Model> m_targetModel;
    std::optional<docgen::Model> m_referenceModel;
    schemadiff::Diff m_diff;
    QString m_script;
    std::vector<std::vector<std::pair<int, int>>> m_lines;
};

} // namespace slonisko
