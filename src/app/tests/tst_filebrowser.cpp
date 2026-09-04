// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditorPage.h"
#include "FileBrowser.h"
#include "MainWindow.h"
#include "Session.h"
#include "SqlEditor.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

using namespace slonisko;

namespace {

void write(const QString &path, const QByteArray &text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

} // namespace

class TestFileBrowser : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        write(m_dir.filePath(QStringLiteral("a.sql")), "SELECT 1;\n");
        write(m_dir.filePath(QStringLiteral("notes.txt")), "x");
        write(m_dir.filePath(QStringLiteral("backup.dump")), "x");
        write(m_dir.filePath(QStringLiteral("Makefile")), "x");
        QVERIFY(QDir(m_dir.path()).mkdir(QStringLiteral("sub")));
        write(m_dir.filePath(QStringLiteral("sub/b.sql")), "SELECT 2;\n");
    }

    void init()
    {
        m_settingsDir.emplace();
        m_settings.emplace(m_settingsDir->filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    }

    void filterAndPath()
    {
        FileBrowser b(*m_settings);
        QCOMPARE(b.filter(), QStringLiteral("*.sql")); // The default.
        QVERIFY(b.setPath(m_dir.path()));
        QCOMPARE(b.path(), QDir(m_dir.path()).absolutePath());
        QCOMPARE(b.pathEdit()->text(), QDir::toNativeSeparators(QDir(m_dir.path()).absolutePath()));
        QTRY_COMPARE(visible(b), (QStringList {QStringLiteral("a.sql"), QStringLiteral("sub")}));

        b.setFilter(QStringLiteral("*.txt;*.sql"));
        QTRY_COMPARE(visible(b),
                     (QStringList {QStringLiteral("a.sql"), QStringLiteral("notes.txt"),
                                   QStringLiteral("sub")}));
        b.setFilter(QString()); // Everything.
        QTRY_VERIFY(visible(b).contains(QStringLiteral("notes.txt")));

        // The fixed choices, in order; choosing one filters by it.
        QComboBox *box = b.filterBox();
        QVERIFY(box->isEditable());
        QCOMPARE(box->count(), 3);
        QCOMPARE(box->itemText(0), QStringLiteral("*.sql"));
        QCOMPARE(box->itemText(1), QStringLiteral("*.dump"));
        QCOMPARE(box->itemText(2), QStringLiteral("*.*"));
        box->setCurrentIndex(1);
        QTRY_COMPARE(visible(b),
                     (QStringList {QStringLiteral("backup.dump"), QStringLiteral("sub")}));
        box->setCurrentIndex(2); // All files, also without a dot.
        QTRY_VERIFY(visible(b).contains(QStringLiteral("Makefile")));
        QVERIFY(visible(b).contains(QStringLiteral("notes.txt")));

        // Typed filters work but do not become choices.
        box->setEditText(QStringLiteral("*.txt"));
        QTRY_COMPARE(visible(b),
                     (QStringList {QStringLiteral("notes.txt"), QStringLiteral("sub")}));
        QTest::keyClick(box->lineEdit(), Qt::Key_Return);
        QCOMPARE(box->count(), 3);

        // Not a folder: refused, the tree stays where it was.
        QVERIFY(!b.setPath(m_dir.filePath(QStringLiteral("a.sql"))));
        QVERIFY(!b.setPath(m_dir.filePath(QStringLiteral("nowhere"))));
        QCOMPARE(b.path(), QDir(m_dir.path()).absolutePath());
    }

    void activatingOpens()
    {
        FileBrowser b(*m_settings);
        QVERIFY(b.setPath(m_dir.path()));
        QSignalSpy opened(&b, &FileBrowser::fileActivated);
        QModelIndex file;
        QTRY_VERIFY((file = b.model()->index(m_dir.filePath(QStringLiteral("a.sql")))).isValid());
        Q_EMIT b.view()->activated(file);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(),
                 QDir(m_dir.path()).absoluteFilePath(QStringLiteral("a.sql")));
        Q_EMIT b.view()->activated(b.model()->index(m_dir.filePath(QStringLiteral("sub"))));
        QCOMPARE(opened.size(), 1); // Folders just expand.
    }

    void remembered()
    {
        {
            FileBrowser b(*m_settings);
            QVERIFY(b.setPath(m_dir.filePath(QStringLiteral("sub"))));
            b.setFilter(QStringLiteral("*.psql"));
        }
        FileBrowser again(*m_settings);
        QCOMPARE(again.path(), QDir(m_dir.filePath(QStringLiteral("sub"))).absolutePath());
        QCOMPARE(again.filter(), QStringLiteral("*.psql"));
    }

    void mainWindowOpensOnce()
    {
        MainWindow w;
        auto *left = qobject_cast<QTabWidget *>(w.files()->parentWidget()->parentWidget());
        QVERIFY(left);
        QCOMPARE(left->tabPosition(), QTabWidget::West);
        const QString path = m_dir.filePath(QStringLiteral("a.sql"));
        EditorPage *first = w.openFile(path);
        QVERIFY(first);
        QCOMPARE(first->editor()->utf8Text(), QByteArray("SELECT 1;\n"));
        EditorPage *again = w.openFile(path);
        QCOMPARE(again, first); // Already open: the same tab.
        QCOMPARE(w.currentEditor(), first);
    }

private:
    static QStringList visible(const FileBrowser &b)
    {
        QStringList out;
        const QModelIndex root = b.view()->rootIndex();
        for (int row = 0; row < b.model()->rowCount(root); ++row)
            out << b.model()->index(row, 0, root).data().toString();
        out.sort();
        return out;
    }

    QTemporaryDir m_dir;
    std::optional<QTemporaryDir> m_settingsDir;
    std::optional<QSettings> m_settings;
};

QTEST_MAIN(TestFileBrowser)
#include "tst_filebrowser.moc"
