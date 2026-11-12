// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditorPage.h"
#include "MainWindow.h"
#include "RecentFiles.h"
#include "Session.h"
#include "SqlEditor.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace slonisko;

namespace {

void write(const QString &path, const QByteArray &text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

// The file actions in the menu, without the separator and Clear List, as
// the menu shows them on opening.
QStringList menuPaths(QMenu *menu)
{
    Q_EMIT menu->aboutToShow();
    QStringList out;
    for (const QAction *action : menu->actions()) {
        if (!action->data().toString().isEmpty())
            out << action->data().toString();
    }
    return out;
}

} // namespace

class TestRecentFiles : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        write(file(QStringLiteral("a.sql")), "SELECT 1;\n");
        write(file(QStringLiteral("b.sql")), "SELECT 2;\n");
        write(file(QStringLiteral("c.sql")), "SELECT 3;\n");
    }

    void init()
    {
        // MainWindow uses the default settings; in test mode they are a
        // throwaway file, but one that lasts from run to run.
        QSettings().remove(QStringLiteral("files/recent"));
        m_settingsDir.emplace();
        m_settings.emplace(m_settingsDir->filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    }

    void order()
    {
        RecentFiles recent(*m_settings);
        QSignalSpy changed(&recent, &RecentFiles::changed);
        QVERIFY(recent.paths().isEmpty());
        recent.add(file(QStringLiteral("a.sql")));
        recent.add(file(QStringLiteral("b.sql")));
        QCOMPARE(recent.paths(),
                 (QStringList {file(QStringLiteral("b.sql")), file(QStringLiteral("a.sql"))}));
        // Again, by another way to the same file: moves up, not listed twice.
        recent.add(m_dir.path() + QStringLiteral("/sub/../a.sql"));
        QCOMPARE(recent.paths(),
                 (QStringList {file(QStringLiteral("a.sql")), file(QStringLiteral("b.sql"))}));
        QCOMPARE(changed.size(), 3);
        recent.add(file(QStringLiteral("a.sql"))); // Already on top.
        QCOMPARE(changed.size(), 3);

        recent.remove(file(QStringLiteral("b.sql")));
        QCOMPARE(recent.paths(), QStringList {file(QStringLiteral("a.sql"))});
        recent.clear();
        QVERIFY(recent.paths().isEmpty());
        QCOMPARE(changed.size(), 5);
    }

    void limitedAndRemembered()
    {
        {
            RecentFiles recent(*m_settings);
            for (int i = 0; i < RecentFiles::MaxCount + 3; ++i)
                recent.add(file(QStringLiteral("f%1.sql").arg(i)));
            QCOMPARE(recent.paths().size(), RecentFiles::MaxCount);
            QCOMPARE(recent.paths().first(),
                     file(QStringLiteral("f%1.sql").arg(RecentFiles::MaxCount + 2)));
        }
        RecentFiles again(*m_settings);
        QCOMPARE(again.paths().size(), RecentFiles::MaxCount);
        QCOMPARE(again.paths().first(),
                 file(QStringLiteral("f%1.sql").arg(RecentFiles::MaxCount + 2)));
    }

    void mainWindowMenu()
    {
        MainWindow w;
        QMenu *menu = w.recentMenu();
        QVERIFY(menu);
        QVERIFY(!menu->isEnabled()); // Nothing yet.

        // Opening and saving under a new name both count.
        EditorPage *a = w.openFile(file(QStringLiteral("a.sql")));
        QVERIFY(a);
        EditorPage *editor = w.newEditor();
        editor->editor()->setText(QStringLiteral("SELECT 4;\n"));
        QVERIFY(editor->saveFile(file(QStringLiteral("d.sql"))));
        QVERIFY(menu->isEnabled());
        QCOMPARE(menuPaths(menu),
                 (QStringList {file(QStringLiteral("d.sql")), file(QStringLiteral("a.sql"))}));
        QVERIFY(menu->actions().first()->text().startsWith(QStringLiteral("&1 d.sql")));

        // Choosing one opens it.
        w.recentFiles()->add(file(QStringLiteral("b.sql")));
        QCOMPARE(menuPaths(menu).first(), file(QStringLiteral("b.sql")));
        QPointer<QAction> chosen = menu->actions().first();
        chosen->trigger();
        QVERIFY(chosen); // Still there: the menu is rebuilt only when it opens.
        auto *b = qobject_cast<EditorPage *>(w.currentPage());
        QVERIFY(b);
        QCOMPARE(b->filePath(), file(QStringLiteral("b.sql")));
        QCOMPARE(b->editor()->utf8Text(), QByteArray("SELECT 2;\n"));

        // One that is gone drops out of the list, after a warning.
        w.recentFiles()->add(file(QStringLiteral("gone.sql")));
        QCOMPARE(menuPaths(menu).first(), file(QStringLiteral("gone.sql")));
        QTimer::singleShot(0, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        menu->actions().first()->trigger();
        QVERIFY(!w.recentFiles()->paths().contains(file(QStringLiteral("gone.sql"))));

        // Clear List empties it.
        menuPaths(menu);
        menu->actions().last()->trigger();
        QVERIFY(w.recentFiles()->paths().isEmpty());
        QVERIFY(!menu->isEnabled());
    }

private:
    QString file(const QString &name) const { return QDir(m_dir.path()).absoluteFilePath(name); }

    QTemporaryDir m_dir;
    std::optional<QTemporaryDir> m_settingsDir;
    std::optional<QSettings> m_settings;
};

QTEST_MAIN(TestRecentFiles)
#include "tst_recentfiles.moc"
