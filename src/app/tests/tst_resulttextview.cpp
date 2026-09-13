// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ResultTextView.h"

#include <QMouseEvent>
#include <QTest>

using namespace slonisko;

class TestResultTextView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_view = std::make_unique<ResultTextView>();
        m_view->setText(QStringLiteral("id | name  | score\n"
                                       "---+-------+------\n"
                                       " 1 | aaaaa |  1.50\n"
                                       " 2 | bbbbb |  3.00\n"
                                       " 3 | ccccc |  4.50\n"));
        m_view->resize(400, 200);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view.get()));
    }

    void dragSelectsLines()
    {
        drag(Qt::NoModifier);
        QVERIFY(m_view->selection().startsWith(QLatin1String("name  | score\n---")));
    }

    // The point of the text view: one column out of the table.
    void altDragSelectsABlock()
    {
        drag(Qt::AltModifier);
        QCOMPARE(m_view->selection(), QStringLiteral("nam\n---\naaa\nbbb\nccc\n"));
    }

    void selectBlockDoesTheSame()
    {
        const qsizetype name = m_view->text().indexOf(QLatin1String("name"));
        const qsizetype lastLine = m_view->text().lastIndexOf(QLatin1Char('\n'), -2) + 1;
        m_view->selectBlock(name, lastLine + name + 3);
        QCOMPARE(m_view->selection(), QStringLiteral("nam\n---\naaa\nbbb\nccc\n"));
    }

    void isReadOnly()
    {
        QVERIFY(m_view->isReadOnly());
        const QString before = m_view->text();
        QTest::keyClicks(m_view.get(), QStringLiteral("nope"));
        QCOMPARE(m_view->text(), before);
    }

    void cleanupTestCase() { m_view.reset(); }

private:
    // From the "n" of "name" on the first line to the last line, as a mouse
    // does it: the modifier is on the move events too.
    void drag(Qt::KeyboardModifiers modifiers)
    {
        const qsizetype name = m_view->text().indexOf(QLatin1String("name"));
        const qsizetype lastLine = m_view->text().lastIndexOf(QLatin1Char('\n'), -2) + 1;
        const QPointF from = pointOf(name);
        const QPointF to = pointOf(lastLine + name + 3);
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, modifiers);
        // Scintilla throttles drag moves, so they need a moment apart.
        send(QEvent::MouseMove, QPointF((from.x() + to.x()) / 2, (from.y() + to.y()) / 2),
             Qt::NoButton, modifiers);
        QTest::qWait(150);
        send(QEvent::MouseMove, to, Qt::NoButton, modifiers);
        QTest::qWait(150);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, modifiers);
    }

    QPointF pointOf(qsizetype position) const
    {
        return QPointF(
            qreal(m_view->SendScintilla(QsciScintilla::SCI_POINTXFROMPOSITION, 0UL, position)),
            qreal(m_view->SendScintilla(QsciScintilla::SCI_POINTYFROMPOSITION, 0UL, position))
                + 2.0);
    }

    void send(QEvent::Type type, const QPointF &at, Qt::MouseButton button,
              Qt::KeyboardModifiers modifiers)
    {
        QWidget *viewport = m_view->viewport();
        QMouseEvent event(type, at, viewport->mapToGlobal(at), button,
                          type == QEvent::MouseMove ? Qt::LeftButton : button, modifiers);
        QCoreApplication::sendEvent(viewport, &event);
    }

    std::unique_ptr<ResultTextView> m_view;
};

QTEST_MAIN(TestResultTextView)
#include "tst_resulttextview.moc"
