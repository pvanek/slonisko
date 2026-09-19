// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTest>

using namespace slonisko;
using catalog::ExportFormat;

class TestExportDialog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultsToTheSelection()
    {
        ExportDialog dialog({.fetched = 10, .selected = 3, .truncated = false},
                            QStringLiteral("public.orders"));
        QCOMPARE(dialog.scope(), ExportDialog::Scope::Selected);
        QCOMPARE(dialog.options().table, QStringLiteral("public.orders"));

        ExportDialog nothingSelected({.fetched = 10, .selected = 0, .truncated = false}, {});
        QCOMPARE(nothingSelected.scope(), ExportDialog::Scope::Fetched);
        QCOMPARE(nothingSelected.options().table, QStringLiteral("table_name"));
    }

    void saysWhenAllRowsMeansRunningAgain()
    {
        ExportDialog dialog({.fetched = 1000, .selected = 0, .truncated = true}, {});
        auto *rows = dialog.findChild<QComboBox *>(QStringLiteral("rows"));
        QVERIFY(rows);
        QVERIFY(rows->itemText(2).contains(QLatin1String("again")));
        rows->setCurrentIndex(2);
        QCOMPARE(dialog.scope(), ExportDialog::Scope::All);
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("note"))
                    ->text()
                    .contains(QLatin1String("1000")));
    }

    // Each format has its own options; the others are greyed out.
    void optionsFollowTheFormat()
    {
        ExportDialog dialog({.fetched = 1, .selected = 0, .truncated = false}, {});
        auto *format = dialog.findChild<QComboBox *>(QStringLiteral("format"));
        auto *delimiter = dialog.findChild<QLineEdit *>(QStringLiteral("delimiter"));
        auto *table = dialog.findChild<QLineEdit *>(QStringLiteral("table"));
        QVERIFY(format && delimiter && table);

        format->setCurrentIndex(format->findData(int(ExportFormat::Csv)));
        QVERIFY(delimiter->isEnabled());
        QVERIFY(!table->isEnabled());

        format->setCurrentIndex(format->findData(int(ExportFormat::SqlBulkInsert)));
        QCOMPARE(dialog.format(), ExportFormat::SqlBulkInsert);
        QVERIFY(!delimiter->isEnabled());
        QVERIFY(table->isEnabled());
        QVERIFY(dialog.findChild<QSpinBox *>(QStringLiteral("bulkRows"))->isEnabled());
    }

    // Regression: neither button was connected, so the dialog never closed.
    void buttonsCloseTheDialog()
    {
        ExportDialog cancelled({.fetched = 1, .selected = 0, .truncated = false}, {});
        cancelled.show();
        auto *buttons = cancelled.findChild<QDialogButtonBox *>(QStringLiteral("buttons"));
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!cancelled.isVisible());
        QCOMPARE(cancelled.result(), int(QDialog::Rejected));

        ExportDialog accepted({.fetched = 1, .selected = 0, .truncated = false}, {});
        accepted.show();
        // To the clipboard, so accepting asks for no file.
        accepted.findChild<QRadioButton *>(QStringLiteral("toClipboard"))->setChecked(true);
        accepted.findChild<QDialogButtonBox *>(QStringLiteral("buttons"))
            ->button(QDialogButtonBox::Ok)
            ->click();
        QVERIFY(!accepted.isVisible());
        QCOMPARE(accepted.result(), int(QDialog::Accepted));
    }

    void readsBackTheOptions()
    {
        ExportDialog dialog({.fetched = 1, .selected = 0, .truncated = false}, {});
        dialog.findChild<QComboBox *>(QStringLiteral("format"))
            ->setCurrentIndex(dialog.findChild<QComboBox *>(QStringLiteral("format"))
                                  ->findData(int(ExportFormat::Csv)));
        dialog.findChild<QLineEdit *>(QStringLiteral("delimiter"))->setText(QStringLiteral(";"));
        dialog.findChild<QLineEdit *>(QStringLiteral("nullText"))->setText(QStringLiteral("\\N"));
        dialog.findChild<QCheckBox *>(QStringLiteral("header"))->setChecked(false);

        const catalog::ExportOptions options = dialog.options();
        QCOMPARE(options.format, ExportFormat::Csv);
        QCOMPARE(options.delimiter, QLatin1Char(';'));
        QCOMPARE(options.nullText, QStringLiteral("\\N"));
        QVERIFY(!options.header);

        QVERIFY(!dialog.toClipboard());
        dialog.findChild<QRadioButton *>(QStringLiteral("toClipboard"))->setChecked(true);
        QVERIFY(dialog.toClipboard());
        QVERIFY(!dialog.findChild<QLineEdit *>(QStringLiteral("path"))->isEnabled());
    }
};

QTEST_MAIN(TestExportDialog)
#include "tst_exportdialog.moc"
