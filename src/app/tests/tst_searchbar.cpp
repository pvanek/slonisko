// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SearchBar.h"
#include "SqlEditor.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QVBoxLayout>

#include <memory>

using namespace slonisko;

namespace {

using Range = std::pair<qsizetype, qsizetype>;

// An editor with its bar below it, the way EditorPage holds them.
struct Area
{
    Area()
    {
        auto *layout = new QVBoxLayout(&widget);
        editor = new SqlEditor(&widget);
        bar = new SearchBar(editor, &widget);
        layout->addWidget(editor);
        layout->addWidget(bar);
        widget.addActions(bar->editorActions());
        widget.resize(800, 400);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        editor->setFocus();
    }

    QString selected() const
    {
        const auto [from, to] = editor->selectionRange();
        return QString::fromUtf8(editor->utf8Text().sliced(from, to - from));
    }

    bool highlightedAt(qsizetype pos) const
    {
        return editor->SendScintilla(QsciScintillaBase::SCI_INDICATORVALUEAT,
                                     static_cast<unsigned long>(SqlEditor::SearchIndicator),
                                     static_cast<long>(pos))
            != 0;
    }

    QCheckBox *check(const char *name) const { return bar->findChild<QCheckBox *>(name); }
    QComboBox *modeBox() const { return bar->findChild<QComboBox *>("mode"); }

    void power(const QString &find, const QString &replace,
               sql::TextSearch::Mode mode = sql::TextSearch::Mode::PlainText)
    {
        bar->openPower();
        modeBox()->setCurrentIndex(int(mode));
        bar->findField()->setEditText(find);
        bar->replaceField()->setEditText(replace);
    }

    QWidget widget;
    SqlEditor *editor = nullptr;
    SearchBar *bar = nullptr;
};

} // namespace

class TestSearchBar : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() { QSettings().remove(QStringLiteral("search")); }

    // Ctrl+F finds as you type, from where the cursor was.
    void incremental()
    {
        Area a;
        a.editor->setText(QStringLiteral("SELECT id FROM t;\nSELECT name FROM t;"));
        a.editor->setCursorPosition(5);
        QTest::keyClick(a.editor, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(a.bar->isVisible());
        QCOMPARE(a.bar->mode(), SearchBar::Mode::Incremental);
        QVERIFY(a.bar->findField()->lineEdit()->hasFocus());
        QVERIFY(!a.bar->replaceField()->isVisible());

        QTest::keyClicks(a.bar->findField()->lineEdit(), QStringLiteral("SEL"));
        // Not the SELECT the cursor was in, but the next one.
        QCOMPARE(a.editor->selectionRange(), Range(18, 21));
        QCOMPARE(a.bar->state(), SearchBar::State::Found);
        QVERIFY(a.highlightedAt(0));
        QVERIFY(a.highlightedAt(18));
        QVERIFY(!a.highlightedAt(7));

        // Enter finds the next one, starting over at the top.
        QTest::keyClick(a.bar->findField()->lineEdit(), Qt::Key_Return);
        QCOMPARE(a.editor->selectionRange(), Range(0, 3));
        QCOMPARE(a.bar->statusText(), QStringLiteral("Continued from the top"));
        QTest::keyClick(a.bar->findField()->lineEdit(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(a.editor->selectionRange(), Range(18, 21));
        QCOMPARE(a.bar->statusText(), QStringLiteral("Continued from the bottom"));

        QTest::keyClicks(a.bar->findField()->lineEdit(), QStringLiteral("x"));
        QCOMPARE(a.bar->state(), SearchBar::State::NotFound);
        QVERIFY(!a.highlightedAt(0));
        QVERIFY(a.bar->findField()->palette().color(QPalette::Base)
                != a.bar->palette().color(QPalette::Base));

        // Esc closes the bar, back in the editor.
        QTest::keyClick(a.bar->findField()->lineEdit(), Qt::Key_Escape);
        QVERIFY(!a.bar->isVisible());
        QVERIFY(a.editor->hasFocus());
    }

    // Typing in the plain incremental search finds regular expression
    // characters as themselves, and match case is honoured.
    void incrementalIsPlainText()
    {
        Area a;
        a.editor->setText(QStringLiteral("select 1.5, 105, SELECT"));
        a.modeBox()->setCurrentIndex(int(sql::TextSearch::Mode::RegularExpression));
        a.bar->openIncremental();
        QTest::keyClicks(a.bar->findField()->lineEdit(), QStringLiteral("1.5"));
        QCOMPARE(a.selected(), QStringLiteral("1.5"));
        QVERIFY(!a.highlightedAt(12));

        a.bar->findField()->setEditText(QStringLiteral("SELECT"));
        a.check("matchCase")->setChecked(true);
        QCOMPARE(a.selected(), QStringLiteral("SELECT"));
        QCOMPARE(a.editor->selectionRange().first, 17);
    }

    // The selection is what to find; the old text is offered, all selected.
    void openWithSelection()
    {
        Area a;
        a.editor->setText(QStringLiteral("a.b + ab + a.b"));
        a.bar->findField()->setEditText(QStringLiteral("old"));
        a.editor->selectRange(0, 3);
        a.bar->openIncremental();
        QCOMPARE(a.bar->findField()->currentText(), QStringLiteral("a.b"));
        QCOMPARE(a.bar->findField()->lineEdit()->selectedText(), QStringLiteral("a.b"));
        QVERIFY(a.highlightedAt(11));
        a.bar->closeBar();
        QVERIFY(!a.highlightedAt(11));

        // With a regular expression, escaped so it finds the selection.
        a.modeBox()->setCurrentIndex(int(sql::TextSearch::Mode::RegularExpression));
        a.editor->selectRange(0, 3);
        a.bar->openPower();
        QCOMPARE(a.bar->findField()->currentText(), QStringLiteral("a\\.b"));
        QVERIFY(!a.check("selectionOnly")->isChecked());
    }

    // F3 and Shift+F3 search again with the bar closed; Ctrl+H for what is
    // selected or the word at the cursor.
    void findAgainAndSelected()
    {
        Area a;
        a.editor->setText(QStringLiteral("id, user_id, id"));
        a.editor->setCursorPosition(1);
        QTest::keyClick(a.editor, Qt::Key_H, Qt::ControlModifier);
        QCOMPARE(a.bar->findField()->currentText(), QStringLiteral("id"));
        QCOMPARE(a.editor->selectionRange(), Range(9, 11));
        QVERIFY(!a.bar->isVisible());

        const QList<QKeySequence> next = QKeySequence::keyBindings(QKeySequence::FindNext);
        const QList<QKeySequence> previous = QKeySequence::keyBindings(QKeySequence::FindPrevious);
        QVERIFY(!next.isEmpty() && !previous.isEmpty());
        QTest::keySequence(a.editor, next.first());
        QCOMPARE(a.editor->selectionRange(), Range(13, 15));
        QTest::keySequence(a.editor, previous.first());
        QCOMPARE(a.editor->selectionRange(), Range(9, 11));
        QTest::keyClick(a.editor, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(a.editor->selectionRange(), Range(0, 2));

        // Nothing to find yet: F3 opens the bar to type it.
        Area b;
        b.bar->findField()->setEditText({});
        QTest::keySequence(b.editor, next.first());
        QVERIFY(b.bar->isVisible());
    }

    // Ctrl+R: Replace replaces a selected match and moves on; one that is
    // not selected yet is found first.
    void replace()
    {
        Area a;
        a.editor->setText(QStringLiteral("x = 1; x = 2;"));
        QTest::keyClick(a.editor, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(a.bar->isVisible());
        QCOMPARE(a.bar->mode(), SearchBar::Mode::Power);
        QVERIFY(a.bar->replaceField()->isVisible());
        a.bar->findField()->setEditText(QStringLiteral("x"));
        a.bar->replaceField()->setEditText(QStringLiteral("y"));

        a.editor->setCursorPosition(3);
        a.bar->replace();
        QCOMPARE(a.editor->text(), QStringLiteral("x = 1; x = 2;"));
        QCOMPARE(a.editor->selectionRange(), Range(7, 8));
        // Enter in the replace field replaces.
        QTest::keyClick(a.bar->replaceField()->lineEdit(), Qt::Key_Return);
        QCOMPARE(a.editor->text(), QStringLiteral("x = 1; y = 2;"));
        QCOMPARE(a.editor->selectionRange(), Range(0, 1));
        a.bar->replace();
        QCOMPARE(a.editor->text(), QStringLiteral("y = 1; y = 2;"));
        QCOMPARE(a.bar->state(), SearchBar::State::NotFound);
    }

    void replaceAllIsOneUndo()
    {
        Area a;
        a.editor->setText(QStringLiteral("SELECT a, b\nFROM t"));
        a.power(QStringLiteral("^"), QStringLiteral("-- "),
                sql::TextSearch::Mode::RegularExpression);
        QCOMPARE(a.bar->replaceAll(), 2);
        QCOMPARE(a.editor->text(), QStringLiteral("-- SELECT a, b\n-- FROM t"));
        QCOMPARE(a.bar->statusText(), QStringLiteral("2 replacement(s)"));
        a.editor->undo();
        QCOMPARE(a.editor->text(), QStringLiteral("SELECT a, b\nFROM t"));

        // Captures and case changes.
        a.power(QStringLiteral("(\\w+) (\\w+)"), QStringLiteral("\\U\\2\\E \\1"),
                sql::TextSearch::Mode::RegularExpression);
        a.editor->setText(QStringLiteral("count id"));
        a.bar->replaceAll();
        QCOMPARE(a.editor->text(), QStringLiteral("ID count"));
    }

    // \n in a replacement is a line break the way the file has them.
    void replacementLineBreaks()
    {
        Area a;
        a.editor->setEolMode(QsciScintilla::EolWindows);
        a.editor->setText(QStringLiteral("a, b"));
        a.power(QStringLiteral(", "), QStringLiteral(",\\n"),
                sql::TextSearch::Mode::EscapeSequences);
        a.bar->replaceAll();
        QCOMPARE(a.editor->utf8Text(), QByteArray("a,\r\nb"));
    }

    // Several lines selected: Ctrl+R keeps the search to them, and the
    // scope follows edits.
    void selectionOnly()
    {
        Area a;
        a.editor->setText(QStringLiteral("t;\nt t;\nt;\nt;"));
        a.editor->selectRange(3, 10);
        a.bar->openPower();
        QVERIFY(a.check("selectionOnly")->isChecked());
        QCOMPARE(a.editor->searchScope(), (std::optional<Range>({3, 10})));
        a.bar->findField()->setEditText(QStringLiteral("t"));
        a.bar->replaceField()->setEditText(QStringLiteral("tab"));
        QCOMPARE(a.bar->findAll(), 3);
        QCOMPARE(a.bar->statusText(), QStringLiteral("3 match(es)"));
        QVERIFY(!a.highlightedAt(0));
        QVERIFY(a.highlightedAt(3));
        QCOMPARE(a.bar->replaceAll(), 3);
        QCOMPARE(a.editor->text(), QStringLiteral("t;\ntab tab;\ntab;\nt;"));
        QCOMPARE(a.editor->searchScope(), (std::optional<Range>({3, 16})));

        // Typed before it: it moves along.
        a.editor->insertAt(QStringLiteral("--"), 0, 0);
        QCOMPARE(a.editor->searchScope(), (std::optional<Range>({5, 18})));

        a.bar->closeBar();
        QVERIFY(!a.check("selectionOnly")->isChecked());
        QVERIFY(!a.editor->searchScope());
    }

    void invalidRegularExpression()
    {
        Area a;
        a.editor->setText(QStringLiteral("(a"));
        a.bar->openPower();
        a.modeBox()->setCurrentIndex(int(sql::TextSearch::Mode::RegularExpression));
        QTest::keyClicks(a.bar->findField()->lineEdit(), QStringLiteral("(a"));
        QCOMPARE(a.bar->state(), SearchBar::State::Invalid);
        QVERIFY(a.bar->statusText().startsWith(QStringLiteral("Invalid regular expression")));
        QVERIFY(!a.bar->findNext());
        QCOMPARE(a.bar->replaceAll(), 0);
        QCOMPARE(a.editor->text(), QStringLiteral("(a"));
    }

    // Matches on show follow edits to the text.
    void highlightFollowsEdits()
    {
        Area a;
        a.editor->setText(QStringLiteral("ab"));
        a.power(QStringLiteral("ab"), {});
        a.bar->findAll();
        QVERIFY(a.highlightedAt(0));
        a.editor->append(QStringLiteral(" ab"));
        QTRY_VERIFY(a.highlightedAt(3));
    }

    // Esc in the editor closes the bar, but closes completion first.
    void escapeInEditor()
    {
        Area a;
        a.bar->openIncremental();
        a.editor->setFocus();
        QTest::keyClick(a.editor, Qt::Key_Escape);
        QVERIFY(!a.bar->isVisible());
    }

    // Esc closes from anywhere in the bar, buttons and boxes too.
    void escapeInBar()
    {
        Area a;
        a.bar->openPower();
        a.check("matchCase")->setFocus();
        QTest::keyClick(a.check("matchCase"), Qt::Key_Escape);
        QVERIFY(!a.bar->isVisible());
        QVERIFY(a.editor->hasFocus());

        a.bar->openPower();
        a.modeBox()->setFocus();
        QTest::keyClick(a.modeBox(), Qt::Key_Escape);
        QVERIFY(!a.bar->isVisible());
    }

    // The key that opened the bar closes it; from the editor it goes back
    // to the bar instead, and the other key switches mode.
    void sameKeyCloses()
    {
        Area a;
        QTest::keyClick(a.editor, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(a.bar->isVisible());
        QLineEdit *field = a.bar->findField()->lineEdit();
        QVERIFY(field->hasFocus());

        a.editor->setFocus();
        QTest::keyClick(a.editor, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(a.bar->isVisible());
        QVERIFY(field->hasFocus());

        QTest::keyClick(field, Qt::Key_R, Qt::ControlModifier);
        QVERIFY(a.bar->isVisible());
        QCOMPARE(a.bar->mode(), SearchBar::Mode::Power);
        a.check("matchCase")->setFocus();
        QTest::keyClick(a.check("matchCase"), Qt::Key_R, Qt::ControlModifier);
        QVERIFY(!a.bar->isVisible());
        QVERIFY(a.editor->hasFocus());

        QTest::keyClick(a.editor, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(field, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(!a.bar->isVisible());
    }

    // What was searched for and with what is there next time.
    void history()
    {
        {
            Area a;
            a.editor->setText(QStringLiteral("a b"));
            a.power(QStringLiteral("a"), QStringLiteral("c"));
            a.check("matchCase")->setChecked(true);
            a.bar->findNext();
            a.bar->findField()->setEditText(QStringLiteral("b"));
            a.bar->replace(); // Finds b first.
            a.bar->replace();
            QCOMPARE(a.editor->text(), QStringLiteral("a c"));
        }
        Area a;
        QCOMPARE(a.bar->findField()->count(), 2);
        QCOMPARE(a.bar->findField()->itemText(0), QStringLiteral("b"));
        QCOMPARE(a.bar->findField()->itemText(1), QStringLiteral("a"));
        QCOMPARE(a.bar->replaceField()->itemText(0), QStringLiteral("c"));
        QVERIFY(a.check("matchCase")->isChecked());
    }

    void readOnly()
    {
        Area a;
        a.editor->setText(QStringLiteral("a a"));
        a.editor->setReadOnly(true);
        a.power(QStringLiteral("a"), QStringLiteral("b"));
        QVERIFY(!a.bar->findChild<QWidget *>("replaceAllButton")->isEnabled());
        QCOMPARE(a.bar->replaceAll(), 0);
        QCOMPARE(a.editor->text(), QStringLiteral("a a"));
    }
};

QTEST_MAIN(TestSearchBar)
#include "tst_searchbar.moc"
