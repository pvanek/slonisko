// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EditorPage.h"
#include "MainWindow.h"
#include "PageTabWidget.h"
#include "SavedEditors.h"
#include "Session.h"
#include "SqlEditor.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace slonisko;

namespace {

void write(const QString &path, const QByteArray &text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

QList<EditorPage *> editors(const MainWindow &w)
{
    QList<EditorPage *> out;
    for (WorkspacePage *page : w.pages()) {
        if (auto *editor = qobject_cast<EditorPage *>(page))
            out << editor;
    }
    return out;
}

} // namespace

class TestSavedEditors : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        write(file(QStringLiteral("a.sql")), "SELECT 1;\nSELECT 2;\n");
    }

    void init()
    {
        // MainWindow uses the default settings; in test mode they are a
        // throwaway file, but one that lasts from run to run.
        QSettings().remove(QStringLiteral("session"));
    }

    void roundTrip()
    {
        QTemporaryDir dir;
        QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
        SavedEditors saved;
        saved.editors << SavedEditor {QString(), QStringLiteral("SELECT 'a';\nSELECT 'b';"), 1, 3}
                      << SavedEditor {file(QStringLiteral("a.sql")), QString(), 0, 0};
        saved.current = 1;
        saved.store(settings);
        const SavedEditors back = SavedEditors::load(settings);
        QCOMPARE(back.editors, saved.editors);
        QCOMPARE(back.current, 1);

        // A shorter list replaces the longer one entirely.
        SavedEditors fewer;
        fewer.editors << saved.editors.first();
        fewer.store(settings);
        QCOMPARE(SavedEditors::load(settings).editors, fewer.editors);
        QCOMPARE(SavedEditors::load(settings).current, -1);
    }

    void quitAndStart()
    {
        {
            MainWindow w;
            w.restoreEditors(); // Nothing saved: just the blank editor.
            QCOMPARE(editors(w).size(), 1);
            QVERIFY(editors(w).first()->isBlank());

            editors(w).first()->editor()->setText(QStringLiteral("SELECT 'kept';\nSELECT 2;"));
            editors(w).first()->editor()->setCursorPosition(1, 4);
            EditorPage *a = w.openFile(file(QStringLiteral("a.sql")));
            QVERIFY(a);
            a->editor()->setCursorPosition(1, 2);
            w.newEditor(); // Blank: not kept.
            w.showPage(a);
            // Text without a file does not ask to be saved: it is kept.
            QVERIFY(w.close());
        }
        MainWindow w;
        w.restoreEditors();
        const QList<EditorPage *> back = editors(w);
        QCOMPARE(back.size(), 2);
        QVERIFY(back[0]->filePath().isEmpty());
        QCOMPARE(back[0]->editor()->text(), QStringLiteral("SELECT 'kept';\nSELECT 2;"));
        QVERIFY(back[0]->isModified()); // Closing its tab still asks.
        QCOMPARE(back[1]->filePath(), file(QStringLiteral("a.sql")));
        QVERIFY(!back[1]->isModified());
        QCOMPARE(w.currentEditor(), back[1]);
        int line = 0;
        int column = 0;
        back[0]->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 1);
        QCOMPARE(column, 4);
        back[1]->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 1);
        QCOMPARE(column, 2);
    }

    void missingFileLeftOut()
    {
        QSettings settings;
        SavedEditors saved;
        saved.editors << SavedEditor {file(QStringLiteral("gone.sql")), QString(), 0, 0}
                      << SavedEditor {QString(), QStringLiteral("SELECT 3;"), 0, 0};
        saved.current = 0;
        saved.store(settings);
        MainWindow w;
        w.restoreEditors();
        QCOMPARE(editors(w).size(), 1);
        QCOMPARE(editors(w).first()->editor()->text(), QStringLiteral("SELECT 3;"));
    }

    void storedWhileWorking()
    {
        MainWindow w;
        w.restoreEditors();
        w.currentEditor()->editor()->setText(QStringLiteral("SELECT 'crash';"));
        // Not only on quitting: soon after a change.
        const auto stored = [] {
            QSettings settings;
            return SavedEditors::load(settings).editors;
        };
        QTRY_COMPARE_WITH_TIMEOUT(stored().size(), 1, 5000);
        QCOMPARE(stored().first().text, QStringLiteral("SELECT 'crash';"));
    }

    void notKeptWithoutRestoring()
    {
        // Windows of other tests and tools neither read nor overwrite them.
        QSettings settings;
        SavedEditors saved;
        saved.editors << SavedEditor {QString(), QStringLiteral("SELECT 4;"), 0, 0};
        saved.store(settings);
        {
            MainWindow w;
            QVERIFY(w.currentEditor()->isBlank());
            w.currentEditor()->editor()->setText(QStringLiteral("SELECT 5;"));
            w.currentEditor()->editor()->setModified(false);
            QVERIFY(w.close());
        }
        QCOMPARE(SavedEditors::load(settings).editors, saved.editors);
    }

private:
    QString file(const QString &name) const { return QDir(m_dir.path()).absoluteFilePath(name); }

    QTemporaryDir m_dir;
};

QTEST_MAIN(TestSavedEditors)
#include "tst_savededitors.moc"
