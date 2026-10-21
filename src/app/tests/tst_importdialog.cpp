// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImportDialog.h"
#include "Session.h"
#include "TestServer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;

class TestImportDialog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // Into an existing table: file columns find table columns by name.
    void mapsColumnsByName()
    {
        ImportDialog dialog(nullptr, {}, QStringLiteral("public"), QStringLiteral("people"));
        dialog.setTableColumns(
            {QStringLiteral("id"), QStringLiteral("first_name"), QStringLiteral("born")},
            {QStringLiteral("integer"), QStringLiteral("text"), QStringLiteral("date")});
        dialog.setPath(write("people.csv", "Id;First Name;Shoe size\n1;Ann;38\n2;Bob;45\n"));

        // The semicolons were noticed.
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("delimiter"))->currentData(),
                 QVariant(QStringLiteral(";")));
        auto *preview = dialog.findChild<QTableWidget *>(QStringLiteral("preview"));
        QCOMPARE(preview->rowCount(), 2);
        QCOMPARE(preview->item(1, 1)->text(), QStringLiteral("Bob"));

        const catalog::ImportPlan plan = dialog.plan();
        QVERIFY(!plan.create);
        QCOMPARE(plan.csv.delimiter, QLatin1Char(';'));
        QCOMPARE(plan.columns.size(), std::size_t(2)); // Shoe size goes nowhere.
        QCOMPARE(plan.columns[0].name, QStringLiteral("id"));
        QCOMPARE(plan.columns[1].source, 1);
        QCOMPARE(plan.columns[1].name, QStringLiteral("first_name"));
        auto *columns = dialog.findChild<QTableWidget *>(QStringLiteral("columns"));
        QCOMPARE(columns->item(1, 3)->text(), QStringLiteral("text")); // The table's type.
        QVERIFY(ok(dialog)->isEnabled());

        // The same table column twice is no import.
        auto *third = qobject_cast<QComboBox *>(columns->cellWidget(2, 2));
        third->setCurrentText(QStringLiteral("id"));
        QCOMPARE(columns->item(2, 0)->checkState(), Qt::Checked);
        QVERIFY(!ok(dialog)->isEnabled());
        QVERIFY(dialog.statusText().contains(QLatin1String("id")));
        third->setCurrentText(QStringLiteral("born"));
        QVERIFY(ok(dialog)->isEnabled());
    }

    // Without a header, by position.
    void mapsColumnsByPosition()
    {
        ImportDialog dialog(nullptr, {}, QStringLiteral("public"), QStringLiteral("t"));
        dialog.setTableColumns({QStringLiteral("a"), QStringLiteral("b")}, {});
        dialog.findChild<QCheckBox *>(QStringLiteral("header"))->setChecked(false);
        dialog.setPath(write("plain.csv", "1,2,3\n4,5,6\n"));
        const catalog::ImportPlan plan = dialog.plan();
        QVERIFY(!plan.csv.header);
        QCOMPARE(plan.columns.size(), std::size_t(2));
        QCOMPARE(plan.columns[1].name, QStringLiteral("b"));
        QCOMPARE(dialog.findChild<QTableWidget *>(QStringLiteral("preview"))->rowCount(), 2);
    }

    // A new table is named after the file and typed after its values.
    void plansANewTable()
    {
        ImportDialog dialog(nullptr, {}, QStringLiteral("sales"), {});
        QVERIFY(dialog.findChild<QRadioButton *>(QStringLiteral("newTable"))->isChecked());
        QVERIFY(!ok(dialog)->isEnabled());
        dialog.setPath(write("Q3 Orders.csv",
                             "Order ID\tPlaced\tTotal\tNote\n"
                             "1\t2026-07-01\t10.5\t\n"
                             "2\t2026-07-02\t3\tfast\n"));
        const catalog::ImportPlan plan = dialog.plan();
        QVERIFY(plan.create);
        QCOMPARE(plan.schema, QStringLiteral("sales"));
        QCOMPARE(plan.table, QStringLiteral("q3_orders"));
        QCOMPARE(plan.csv.delimiter, QLatin1Char('\t'));
        QCOMPARE(plan.columns.size(), std::size_t(4));
        QCOMPARE(plan.columns[0].name, QStringLiteral("order_id"));
        QCOMPARE(plan.columns[0].type, QStringLiteral("integer"));
        QCOMPARE(plan.columns[1].type, QStringLiteral("date"));
        QCOMPARE(plan.columns[2].type, QStringLiteral("numeric"));
        QCOMPARE(plan.columns[3].type, QStringLiteral("text"));
        QVERIFY(ok(dialog)->isEnabled());

        // Leaving a column out.
        dialog.findChild<QTableWidget *>(QStringLiteral("columns"))
            ->item(3, 0)
            ->setCheckState(Qt::Unchecked);
        QCOMPARE(dialog.plan().columns.size(), std::size_t(3));
    }

    void saysWhatIsWrongWithTheFile()
    {
        ImportDialog dialog(nullptr, {}, {}, {});
        dialog.setPath(write("broken.csv", "a,b\n1,\"open\n"));
        QVERIFY(!ok(dialog)->isEnabled());
        QVERIFY2(dialog.statusText().contains(QLatin1String("quote")),
                 qPrintable(dialog.statusText()));
    }

    // The whole way, through a session: the table's columns come from the
    // server, the import runs on a connection of its own.
    void imports()
    {
        const std::optional<TestServer> server = TestServer::find();
        if (!server)
            QSKIP("No test server: run through ctest with Docker, or set SLONISKO_TEST_CONNINFO");
        Session session(server->profile, Session::Credentials {server->password, {}});
        session.open();
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QVERIFY(run(session,
                    "DROP TABLE IF EXISTS slonisko_import_dialog;"
                    "CREATE TABLE slonisko_import_dialog (id int, name text)"));

        ImportDialog dialog(&session, {}, QStringLiteral("public"),
                            QStringLiteral("slonisko_import_dialog"));
        QSignalSpy imported(&dialog, &ImportDialog::imported);
        dialog.setPath(write("in.csv", "name,id\nAnn,1\nBob,2\n"));
        QTRY_VERIFY(ok(dialog)->isEnabled()); // Once the columns are read.
        ok(dialog)->click();
        QVERIFY(dialog.isRunning());
        QTRY_COMPARE_WITH_TIMEOUT(imported.size(), 1, 20'000);
        QVERIFY(!dialog.isRunning());
        QVERIFY2(dialog.statusText().contains(QLatin1String("2 rows")),
                 qPrintable(dialog.statusText()));
        QVERIFY(
            run(session, "SELECT 1 FROM slonisko_import_dialog WHERE id = 2 AND name = 'Bob'", 1));

        // Into a new table, then, which exists after.
        dialog.findChild<QRadioButton *>(QStringLiteral("newTable"))->setChecked(true);
        dialog.findChild<QLineEdit *>(QStringLiteral("table"))
            ->setText(QStringLiteral("slonisko_import_dialog_new"));
        QVERIFY(run(session, "DROP TABLE IF EXISTS slonisko_import_dialog_new"));
        QTRY_VERIFY(ok(dialog)->isEnabled());
        ok(dialog)->click();
        QTRY_COMPARE_WITH_TIMEOUT(imported.size(), 2, 20'000);
        QVERIFY(dialog.findChild<QRadioButton *>(QStringLiteral("existingTable"))->isChecked());
        QVERIFY(run(session, "SELECT 1 FROM slonisko_import_dialog_new WHERE id = 1", 1));
        QVERIFY(run(session,
                    "DROP TABLE slonisko_import_dialog; DROP TABLE slonisko_import_dialog_new"));
    }

private:
    static QPushButton *ok(ImportDialog &dialog)
    {
        return dialog.findChild<QDialogButtonBox *>(QStringLiteral("buttons"))
            ->button(QDialogButtonBox::Ok);
    }

    QString write(const char *name, const QByteArray &contents)
    {
        const QString path = m_dir.filePath(QString::fromUtf8(name));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(contents);
        return path;
    }

    // True if sql ran, and returned rows rows when that is not -1.
    static bool run(Session &session, const QByteArray &sql, int rows = -1)
    {
        std::optional<pg::QueryOutcome> outcome;
        QObject context;
        session.runner()->run(sql, &context, [&](const pg::QueryOutcome &o) { outcome = o; });
        if (!QTest::qWaitFor([&] { return outcome.has_value(); }, 10'000) || !outcome->ok()) {
            qWarning() << (outcome ? outcome->error : QStringLiteral("timed out"));
            return false;
        }
        return rows < 0 || outcome->results.back().rowCount() == rows;
    }

    QTemporaryDir m_dir;
};

QTEST_MAIN(TestImportDialog)
#include "tst_importdialog.moc"
