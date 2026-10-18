// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Generator.h"
#include "quality/Report.h"

#include <QPointer>
#include <QWizard>
#include <QWizardPage>

#include <atomic>
#include <functional>
#include <memory>

class QCheckBox;
class QTimer;

namespace Ui {
class DocGenWizard;
}

namespace slonisko {

class Session;

// A page of the wizard that is complete when the wizard says so, as
// Designer's pages have no say of their own.
class DocGenPage : public QWizardPage
{
    Q_OBJECT

public:
    using QWizardPage::QWizardPage;

    void setComplete(std::function<bool()> complete) { m_complete = std::move(complete); }
    bool isComplete() const override { return m_complete ? m_complete() : true; }
    void updateComplete() { Q_EMIT completeChanged(); }

private:
    std::function<bool()> m_complete;
};

// Generate Schema Documentation on a database: what slonisko-docgen does,
// on a connected session, its tunnel and password included. The schemas,
// the format and folder, what the pages hold, and then the writing.
//
// The Database Quality Report is written the same way, with the same
// formats, so the same wizard writes it: what slonisko-quality-report does.
class DocGenWizard : public QWizard
{
    Q_OBJECT

public:
    enum PageId { SchemasPage, OutputPage, ContentsPage, GeneratePage };
    enum class Kind { Documentation, QualityReport };

    // For the database the browser's context menu was opened on; empty
    // means the profile's own.
    DocGenWizard(Session *session, const QString &database, QWidget *parent = nullptr,
                 Kind kind = Kind::Documentation);
    ~DocGenWizard() override;

    Kind kind() const { return m_kind; }
    Session *session() const;
    QString database() const { return m_database; }
    docgen::Options options() const;
    // The same choices, for the quality report.
    quality::Options reportOptions() const;
    bool isRunning() const { return m_running; }
    const docgen::WriteResult &lastResult() const { return m_result; }

    // How to ask before writing into a folder that has files; tests answer
    // instead of a message box.
    void setConfirm(std::function<bool(const QString &question)> confirm)
    {
        m_confirm = std::move(confirm);
    }

    bool validateCurrentPage() override;

Q_SIGNALS:
    void generated(bool ok);

protected:
    void initializePage(int id) override;
    void reject() override;

private:
    void loadSchemas();
    void updateAntora();
    void browse();
    // Reads the catalog and writes the pages, without blocking.
    void generate();
    void setRunning(bool running);
    void finish(const docgen::WriteResult &result, const QString &summary = {});
    void fail(const QString &message);
    void saveSettings() const;

    void generateReport(pg::QueryRunner *runner);
    QString settingsGroup() const;
    QList<QCheckBox *> remembered() const;

    std::unique_ptr<Ui::DocGenWizard> m_ui; // Layout in DocGenWizard.ui.
    Kind m_kind;
    QPointer<Session> m_session;
    QString m_database;
    std::function<bool(const QString &question)> m_confirm;
    bool m_running = false;
    bool m_succeeded = false;
    // Pages written so far, counted by the thread writing them.
    std::shared_ptr<std::atomic<int>> m_done;
    std::shared_ptr<std::atomic<int>> m_total;
    QTimer *m_progressTimer = nullptr;
    docgen::WriteResult m_result;
};

} // namespace slonisko
