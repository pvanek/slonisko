// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultTextView.h"
#include "ValueLexer.h"
#include "ValueViewer.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace slonisko;
using catalog::ValueSyntax;
using Kind = catalog::ValueToken::Kind;

namespace {

constexpr Oid Text = 25;
constexpr Oid Jsonb = 3802;
constexpr Oid Int4Array = 1007;

// The style Scintilla gave the first occurrence of what, styling it all first.
int styleOf(ValueViewer &viewer, const QString &what)
{
    ResultTextView *editor = viewer.editor();
    editor->SendScintilla(QsciScintilla::SCI_COLOURISE, 0, -1);
    const qsizetype at = viewer.text().toUtf8().indexOf(what.toUtf8());
    if (at < 0)
        return -1;
    return int(
        editor->SendScintilla(QsciScintilla::SCI_GETSTYLEAT, static_cast<unsigned long>(at)));
}

} // namespace

class TestValueViewer : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }
    void init() { QSettings().remove(QStringLiteral("valueViewer")); }

    void formatsAndHighlightsJson()
    {
        ValueViewer viewer(QStringLiteral("doc, row 1"), Jsonb, R"({"name": "ü", "n": [1, true]})",
                           nullptr);
        QCOMPARE(viewer.windowTitle(), QStringLiteral("doc, row 1"));
        QCOMPARE(viewer.syntax(), ValueSyntax::Json);
        QVERIFY(viewer.isFormatted());
        QCOMPARE(viewer.text(),
                 QStringLiteral("{\n    \"name\": \"ü\",\n    \"n\": [\n        1,\n"
                                "        true\n    ]\n}"));
        QCOMPARE(styleOf(viewer, QStringLiteral("\"name\"")), ValueLexer::styleFor(Kind::Key));
        // After a two-byte character: positions are bytes, not characters.
        QCOMPARE(styleOf(viewer, QStringLiteral("\"n\"")), ValueLexer::styleFor(Kind::Key));
        QCOMPARE(styleOf(viewer, QStringLiteral("\"ü\"")), ValueLexer::styleFor(Kind::String));
        QCOMPARE(styleOf(viewer, QStringLiteral("1")), ValueLexer::styleFor(Kind::Number));
        QCOMPARE(styleOf(viewer, QStringLiteral("true")), ValueLexer::styleFor(Kind::Literal));
        QCOMPARE(styleOf(viewer, QStringLiteral(": ")), int(ValueLexer::Default) + 1); // The colon.
        QCOMPARE(styleOf(viewer, QStringLiteral("    ")), int(ValueLexer::Default));
        QCOMPARE(viewer.status(), QStringLiteral("29 characters, 7 line(s)"));

        // Unformatted: as the server sent it, still highlighted.
        viewer.setFormatted(false);
        QCOMPARE(viewer.text(), QStringLiteral(R"({"name": "ü", "n": [1, true]})"));
        QCOMPARE(styleOf(viewer, QStringLiteral("true")), ValueLexer::styleFor(Kind::Literal));
    }

    void formatsArrays()
    {
        ValueViewer viewer(QStringLiteral("a"), Int4Array, "{{1,2},{3,NULL}}", nullptr);
        QCOMPARE(viewer.syntax(), ValueSyntax::Array);
        QCOMPARE(viewer.text(), QStringLiteral("{\n    {1,2},\n    {3,NULL}\n}"));
        QCOMPARE(styleOf(viewer, QStringLiteral("NULL")), ValueLexer::styleFor(Kind::Literal));
        QCOMPARE(styleOf(viewer, QStringLiteral("2")), ValueLexer::styleFor(Kind::Number));
    }

    void plainTextIsLeftAlone()
    {
        const QByteArray text = "line one\n  {not: json}\n" + QByteArray(5000, 'x');
        ValueViewer viewer(QStringLiteral("t"), Text, text, nullptr);
        QCOMPARE(viewer.syntax(), ValueSyntax::Text);
        QVERIFY(!viewer.findChild<QCheckBox *>(QStringLiteral("format"))->isEnabled());
        QCOMPARE(viewer.text(), QString::fromUtf8(text));
        QCOMPARE(styleOf(viewer, QStringLiteral("{")), int(ValueLexer::Default));
        QCOMPARE(viewer.status(), QStringLiteral("5,023 characters, 3 line(s)"));
    }

    void whatDoesNotParseIsShownAsItIs()
    {
        ValueViewer viewer(QStringLiteral("t"), Text, R"({"a": 1)", nullptr);
        QCOMPARE(viewer.syntax(), ValueSyntax::Text); // Not JSON, so not guessed.
        viewer.setSyntax(ValueSyntax::Json);
        QCOMPARE(viewer.text(), QStringLiteral(R"({"a": 1)"));
        QCOMPARE(viewer.status(), QStringLiteral("Not valid JSON, so shown as it is."));
        QCOMPARE(styleOf(viewer, QStringLiteral("\"a\"")), ValueLexer::styleFor(Kind::Key));

        viewer.setSyntax(ValueSyntax::Array);
        QCOMPARE(viewer.status(), QStringLiteral("Not a well-formed array, so shown as it is."));
    }

    void copiesWhatIsShown()
    {
        ValueViewer viewer(QStringLiteral("doc"), Jsonb, R"({"a": 1})", nullptr);
        viewer.findChild<QPushButton *>(QStringLiteral("copy"))->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("{\n    \"a\": 1\n}"));
    }

    void remembersFormatAndWrap()
    {
        {
            ValueViewer viewer(QStringLiteral("doc"), Jsonb, "[1]", nullptr);
            QVERIFY(!viewer.wrapsLines());
            viewer.findChild<QCheckBox *>(QStringLiteral("format"))->click();
            viewer.findChild<QCheckBox *>(QStringLiteral("wrap"))->click();
            QVERIFY(!viewer.isFormatted());
            QCOMPARE(viewer.editor()->wrapMode(), QsciScintilla::WrapWord);
        }
        ValueViewer viewer(QStringLiteral("doc"), Jsonb, "[1]", nullptr);
        QVERIFY(!viewer.isFormatted());
        QVERIFY(viewer.wrapsLines());
        QCOMPARE(viewer.text(), QStringLiteral("[1]"));
    }
    void marginFollowsThePalette()
    {
        auto lineNumbers = [](ValueViewer &viewer) {
            const long bgr = viewer.editor()->SendScintilla(QsciScintilla::SCI_STYLEGETBACK,
                                                            QsciScintilla::STYLE_LINENUMBER);
            return QColor(int(bgr & 0xff), int((bgr >> 8) & 0xff), int((bgr >> 16) & 0xff));
        };
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Base, QColor(30, 30, 30));
        dark.setColor(QPalette::Window, QColor(45, 45, 45));
        dark.setColor(QPalette::Text, QColor(220, 220, 220));

        QApplication::setPalette(dark);
        {
            ValueViewer viewer(QStringLiteral("doc"), Jsonb, "[1]", nullptr);
            QVERIFY(lineNumbers(viewer).lightness() < 64);
        }
        // Switched while open: the margin goes along, as the editor's does.
        ValueViewer viewer(QStringLiteral("doc"), Jsonb, "[1]", nullptr);
        QApplication::setPalette(light);
        QTRY_VERIFY(lineNumbers(viewer).lightness() > 192);
    }
};

QTEST_MAIN(TestValueViewer)
#include "tst_valueviewer.moc"
