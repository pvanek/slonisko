// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SqlEditor.h"

#include "CompletionPopup.h"
#include "SqlLexer.h"
#include "sql/Splitter.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPalette>
#include <QToolTip>
#include <QtConcurrent/QtConcurrentRun>

namespace slonisko {

using sql::StatementSpan;

SqlEditor::SqlEditor(QWidget *parent)
    : QsciScintilla(parent), m_lexer(new SqlLexer(this)), m_popup(new CompletionPopup(this))
{
    setUtf8(true);
    setLexer(m_lexer);
    const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(font);

    // Line numbers.
    setMarginType(0, NumberMargin);
    setMarginLineNumbers(0, true);
    setMarginsFont(font);
    setMarginWidth(1, 0);
    connect(this, &QsciScintilla::linesChanged, this, &SqlEditor::updateMarginWidth);
    updateMarginWidth();

    setFolding(BoxedTreeFoldStyle, 2);
    setIndentationsUseTabs(false);
    setTabWidth(4);
    setAutoIndent(true);
    setBraceMatching(SloppyBraceMatch);
    setCaretLineVisible(true);
    const QPalette palette = QGuiApplication::palette();
    const bool dark = palette.color(QPalette::Base).lightness() < 128;
    setCaretLineBackgroundColor(dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 10));
    setCaretForegroundColor(palette.color(QPalette::Text));
    setMatchedBraceBackgroundColor(dark ? QColor(80, 80, 40) : QColor(255, 240, 160));
    SendScintilla(SCI_SETMULTIPLESELECTION, 1);
    SendScintilla(SCI_SETADDITIONALSELECTIONTYPING, 1);

    // The statement Run would execute: a faint box.
    SendScintilla(SCI_INDICSETSTYLE, StatementIndicator, INDIC_FULLBOX);
    SendScintilla(SCI_INDICSETFORE, StatementIndicator,
                  dark ? QColor(120, 160, 255) : QColor(60, 110, 220));
    SendScintilla(SCI_INDICSETALPHA, StatementIndicator, dark ? 22 : 14);
    SendScintilla(SCI_INDICSETUNDER, StatementIndicator, 1);

    SendScintilla(SCI_INDICSETSTYLE, ErrorIndicator, INDIC_SQUIGGLEPIXMAP);
    SendScintilla(SCI_INDICSETFORE, ErrorIndicator, QColor(220, 40, 40));

    m_statementTimer.setSingleShot(true);
    m_statementTimer.setInterval(100);
    connect(&m_statementTimer, &QTimer::timeout, this, &SqlEditor::updateStatementMark);
    connect(this, &QsciScintilla::cursorPositionChanged, &m_statementTimer,
            qOverload<>(&QTimer::start));
    connect(this, &QsciScintilla::textChanged, this, [this] {
        ++m_revision;
        clearErrors();
        m_statementTimer.start();
    });

    // Semantic highlighting and hover explanations.
    setupSemanticIndicators();
    m_semanticTimer.setSingleShot(true);
    m_semanticTimer.setInterval(200);
    connect(&m_semanticTimer, &QTimer::timeout, this, &SqlEditor::analyzeVisible);
    connect(this, &QsciScintilla::textChanged, &m_semanticTimer, qOverload<>(&QTimer::start));
    connect(this, &QsciScintillaBase::SCN_UPDATEUI, this, [this](int updated) {
        if (updated & SC_UPDATE_V_SCROLL)
            m_semanticTimer.start();
    });
    connect(&m_semanticWatcher, &QFutureWatcher<std::vector<catalog::SemanticSpan>>::finished, this,
            &SqlEditor::applySemantics);
    send(SCI_SETMOUSEDWELLTIME, 500);
    connect(this, &QsciScintillaBase::SCN_DWELLSTART, this, [this](int position, int x, int y) {
        const QString text = position >= 0 ? explanationAt(position) : QString();
        if (!text.isEmpty())
            QToolTip::showText(viewport()->mapToGlobal(QPoint(x, y)), text, viewport());
    });
    connect(this, &QsciScintillaBase::SCN_DWELLEND, this, [] { QToolTip::hideText(); });

    m_completionTimer.setSingleShot(true);
    connect(&m_completionTimer, &QTimer::timeout, this, [this] { complete(false); });
    connect(&m_completionWatcher, &QFutureWatcher<catalog::Completion>::finished, this,
            &SqlEditor::showCompletion);
    connect(m_popup, &CompletionPopup::accepted, this, &SqlEditor::applyCompletion);
}

std::pair<qsizetype, qsizetype> SqlEditor::statementBounds(const QByteArray &text, qsizetype pos)
{
    qsizetype start = 0;
    qsizetype end = text.size();
    for (const StatementSpan &s : sql::splitStatements(text)) {
        qsizetype after = s.offset + s.length; // Past its semicolon, if it has one.
        if (s.kind == StatementSpan::Kind::Sql && s.terminated)
            after = text.indexOf(';', after) + 1;
        if (s.terminated && after <= pos) {
            start = after;
        } else if (s.offset > pos) {
            end = s.offset;
            break;
        }
    }
    return {start, end};
}

void SqlEditor::complete(bool explicitRequest)
{
    const QByteArray text = utf8Text();
    const qsizetype cursor = cursorPosition();
    const auto [start, end] = statementBounds(text, cursor);
    m_statementStart = start;
    m_requestRevision = m_revision;
    m_requestCursor = cursor;
    m_explicitRequest = explicitRequest;

    const QByteArray statement = text.sliced(start, end - start);
    const catalog::SnapshotPtr snapshot = m_snapshot ? m_snapshot() : nullptr;
    m_completionWatcher.setFuture(QtConcurrent::run([statement, cursor = cursor - start, snapshot] {
        static const catalog::Snapshot empty;
        return catalog::complete(statement, cursor, snapshot ? *snapshot : empty);
    }));
}

void SqlEditor::showCompletion()
{
    const catalog::Completion c = m_completionWatcher.result();
    // Typed on, or moved away, meanwhile: a newer request will follow, if any.
    if (m_requestRevision != m_revision || m_requestCursor != cursorPosition())
        return;

    bool show = !c.items.empty() && c.context != catalog::Completion::Context::None;
    if (show && !m_explicitRequest) {
        // Unasked, only once a word is under way (or after a dot), and not
        // when the word is already complete.
        const QByteArray text = utf8Text();
        const bool afterDot = m_requestCursor > 0 && text[m_requestCursor - 1] == '.';
        show = (c.prefix.size() >= 2 || afterDot)
            && !(c.items.size() == 1
                 && c.items.front().label.compare(c.prefix, Qt::CaseInsensitive) == 0);
    }
    if (!show) {
        m_popup->hide();
        return;
    }

    const qsizetype at = m_statementStart + c.replaceFrom;
    const int line = int(send(SCI_LINEFROMPOSITION, at));
    const int x = int(send(SCI_POINTXFROMPOSITION, 0, at));
    const int y = int(send(SCI_POINTYFROMPOSITION, 0, at)) + int(send(SCI_TEXTHEIGHT, line));
    m_completion = c;
    m_popup->showItems(c.items, viewport()->mapToGlobal(QPoint(x, y)));
}

void SqlEditor::applyCompletion(const catalog::CompletionItem &item)
{
    const qsizetype from = m_statementStart + m_completion.replaceFrom;
    const qsizetype to = m_statementStart + m_completion.replaceTo;
    const QByteArray text = item.insertText.toUtf8();
    beginUndoAction();
    send(SCI_SETTARGETSTART, from);
    send(SCI_SETTARGETEND, to);
    SendScintilla(SCI_REPLACETARGET, static_cast<unsigned long>(text.size()), text.constData());
    endUndoAction();
    setCursorPosition(from + text.size());
}

void SqlEditor::keyPressEvent(QKeyEvent *event)
{
    if (m_popup->handleKey(event))
        return;
    if (event->key() == Qt::Key_Space && event->modifiers() & Qt::ControlModifier) {
        complete(true);
        return;
    }
    QsciScintilla::keyPressEvent(event);

    const QString typed = event->text();
    const bool word = typed.size() == 1
        && (typed[0].isLetterOrNumber() || typed[0] == QLatin1Char('_')
            || typed[0] == QLatin1Char('.') || typed[0] == QLatin1Char('"'));
    if (word || (event->key() == Qt::Key_Backspace && m_popup->isVisible())) {
        // Quick while the list is up, so it follows the typing.
        m_completionTimer.start(m_popup->isVisible() ? 30 : 150);
    } else if (!typed.isEmpty() || event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
               || event->key() == Qt::Key_Home || event->key() == Qt::Key_End) {
        m_completionTimer.stop();
        m_popup->hide();
    }
}

void SqlEditor::focusOutEvent(QFocusEvent *event)
{
    if (!m_popup->underMouse())
        m_popup->hide();
    QsciScintilla::focusOutEvent(event);
}

QByteArray SqlEditor::utf8Text() const
{
    const auto length = qsizetype(send(SCI_GETLENGTH));
    return QByteArray(static_cast<const char *>(SendScintillaPtrResult(SCI_GETCHARACTERPOINTER)),
                      length);
}

qsizetype SqlEditor::cursorPosition() const
{
    return qsizetype(send(SCI_GETCURRENTPOS));
}

void SqlEditor::setCursorPosition(qsizetype pos)
{
    send(SCI_GOTOPOS, pos);
}

void SqlEditor::selectRange(qsizetype from, qsizetype to)
{
    send(SCI_SETSEL, from, to);
}

std::vector<ScriptPiece> SqlEditor::pieces(qsizetype from, qsizetype to) const
{
    const QByteArray text = utf8Text().sliced(from, to - from);
    const auto spans = sql::splitStatements(text);
    std::vector<ScriptPiece> out;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const StatementSpan &s = spans[i];
        if (s.kind == StatementSpan::Kind::CopyData)
            continue; // Taken with its COPY below.
        ScriptPiece piece;
        piece.sql = text.mid(s.offset, s.length);
        piece.offset = from + s.offset;
        piece.psqlCommand = s.kind == StatementSpan::Kind::PsqlCommand;
        if (i + 1 < spans.size() && spans[i + 1].kind == StatementSpan::Kind::CopyData)
            piece.copyData = text.mid(spans[i + 1].offset, spans[i + 1].length);
        out.push_back(std::move(piece));
    }
    return out;
}

std::vector<ScriptPiece> SqlEditor::piecesToRun() const
{
    const qsizetype selectionStart = qsizetype(send(SCI_GETSELECTIONSTART));
    const qsizetype selectionEnd = qsizetype(send(SCI_GETSELECTIONEND));
    if (selectionEnd > selectionStart)
        return pieces(selectionStart, selectionEnd);

    const QByteArray text = utf8Text();
    const auto spans = sql::splitStatements(text);
    const int i = sql::statementAt(text, spans, cursorPosition());
    if (i < 0)
        return {};
    const StatementSpan &s = spans[std::size_t(i)];
    ScriptPiece piece;
    piece.sql = text.mid(s.offset, s.length);
    piece.offset = s.offset;
    piece.psqlCommand = s.kind == StatementSpan::Kind::PsqlCommand;
    if (std::size_t(i) + 1 < spans.size()
        && spans[std::size_t(i) + 1].kind == StatementSpan::Kind::CopyData)
        piece.copyData
            = text.mid(spans[std::size_t(i) + 1].offset, spans[std::size_t(i) + 1].length);
    return {piece};
}

std::optional<std::pair<qsizetype, qsizetype>> SqlEditor::currentStatementRange() const
{
    const QByteArray text = utf8Text();
    const auto spans = sql::splitStatements(text);
    const int i = sql::statementAt(text, spans, cursorPosition());
    if (i < 0)
        return std::nullopt;
    const StatementSpan &s = spans[std::size_t(i)];
    return std::pair {s.offset, s.offset + s.length};
}

void SqlEditor::updateStatementMark()
{
    const long length = send(SCI_GETLENGTH);
    send(SCI_SETINDICATORCURRENT, StatementIndicator);
    send(SCI_INDICATORCLEARRANGE, 0, length);
    const bool selection = send(SCI_GETSELECTIONEND) > send(SCI_GETSELECTIONSTART);
    if (selection)
        return; // The selection itself shows what runs.
    if (const auto range = currentStatementRange())
        send(SCI_INDICATORFILLRANGE, range->first, range->second - range->first);
}

void SqlEditor::markError(qsizetype pos, const QString &message)
{
    const long length = send(SCI_GETLENGTH);
    pos = std::clamp<qsizetype>(pos, 0, length);
    qsizetype end = qsizetype(send(SCI_WORDENDPOSITION, pos, true));
    if (end <= pos)
        end = std::min<qsizetype>(pos + 1, length);
    if (end <= pos && pos > 0) // At the very end: mark the last character.
        --pos;
    send(SCI_SETINDICATORCURRENT, ErrorIndicator);
    send(SCI_INDICATORFILLRANGE, pos, std::max<qsizetype>(end - pos, 1));
    m_errors.push_back({{pos, std::max(end, pos + 1)}, message});
}

void SqlEditor::clearErrors()
{
    m_errors.clear();
    send(SCI_SETINDICATORCURRENT, ErrorIndicator);
    send(SCI_INDICATORCLEARRANGE, 0, send(SCI_GETLENGTH));
}

bool SqlEditor::hasErrorAt(qsizetype pos) const
{
    return send(SCI_INDICATORVALUEAT, ErrorIndicator, pos) != 0;
}

int SqlEditor::indicatorFor(catalog::SemanticSpan::Kind kind)
{
    return FirstSemanticIndicator + int(kind);
}

void SqlEditor::setupSemanticIndicators()
{
    using K = catalog::SemanticSpan::Kind;
    const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    auto textColor = [&](K kind, const QColor &color) {
        send(SCI_INDICSETSTYLE, indicatorFor(kind), INDIC_TEXTFORE);
        SendScintilla(SCI_INDICSETFORE, static_cast<unsigned long>(indicatorFor(kind)), color);
    };
    auto pick = [dark](const char *light, const char *darkColor) {
        return QColor::fromString(QLatin1String(dark ? darkColor : light));
    };
    textColor(K::Relation, pick("#00796b", "#4fd1c5"));
    textColor(K::Cte, pick("#00796b", "#4fd1c5"));
    textColor(K::Column, pick("#8a4b08", "#e3a86c"));
    textColor(K::Function, pick("#6f42c1", "#b392f0"));
    for (const K kind : {K::UnknownRelation, K::UnknownColumn}) {
        send(SCI_INDICSETSTYLE, indicatorFor(kind), INDIC_SQUIGGLEPIXMAP);
        SendScintilla(SCI_INDICSETFORE, static_cast<unsigned long>(indicatorFor(kind)),
                      pick("#d98c00", "#f2b84b"));
    }
    // Another language's body: plain text first, its own tokens over it.
    textColor(K::ForeignText, QGuiApplication::palette().color(QPalette::Text));
    textColor(K::ForeignKeyword, SqlLexer::color(SqlLexer::Keyword, dark));
    textColor(K::ForeignString, SqlLexer::color(SqlLexer::String, dark));
    textColor(K::ForeignComment, SqlLexer::color(SqlLexer::Comment, dark));
    textColor(K::ForeignNumber, SqlLexer::color(SqlLexer::Number, dark));
}

void SqlEditor::refreshSemantics()
{
    m_semanticTimer.start(0);
}

void SqlEditor::analyzeVisible()
{
    // The lines on screen, and a margin, so scrolling a little finds them done.
    const auto first = qsizetype(send(SCI_DOCLINEFROMVISIBLE, send(SCI_GETFIRSTVISIBLELINE)));
    const auto count = qsizetype(send(SCI_LINESONSCREEN));
    const qsizetype lastLine = std::max<qsizetype>(lines() - 1, 0);
    const auto from = qsizetype(send(SCI_POSITIONFROMLINE, std::max<qsizetype>(first - 50, 0)));
    const auto to = qsizetype(send(SCI_GETLINEENDPOSITION, std::min(first + count + 50, lastLine)));

    m_semanticRevision = m_revision;
    const catalog::SnapshotPtr snapshot = m_snapshot ? m_snapshot() : nullptr;
    m_semanticWatcher.setFuture(QtConcurrent::run([text = utf8Text(), snapshot, from, to] {
        return catalog::analyzeScript(text, snapshot.get(), from, to);
    }));
}

void SqlEditor::applySemantics()
{
    if (m_semanticRevision != m_revision)
        return; // Edited meanwhile; another analysis is on its way.
    m_semantic = m_semanticWatcher.result();
    const long length = send(SCI_GETLENGTH);
    for (int kind = 0; kind <= int(catalog::SemanticSpan::Kind::ForeignNumber); ++kind) {
        send(SCI_SETINDICATORCURRENT, FirstSemanticIndicator + kind);
        send(SCI_INDICATORCLEARRANGE, 0, length);
    }
    for (const catalog::SemanticSpan &s : m_semantic) {
        send(SCI_SETINDICATORCURRENT, indicatorFor(s.kind));
        send(SCI_INDICATORFILLRANGE, s.offset, s.length);
    }
}

QString SqlEditor::explanationAt(qsizetype pos) const
{
    for (const auto &[range, message] : m_errors) {
        if (pos >= range.first && pos < range.second && !message.isEmpty())
            return message;
    }
    for (const catalog::SemanticSpan &s : m_semantic) {
        if (pos >= s.offset && pos < s.end() && !s.detail.isEmpty()
            && s.kind != catalog::SemanticSpan::Kind::ForeignText)
            return s.detail;
    }
    return {};
}

void SqlEditor::updateMarginWidth()
{
    const QString digits = QString::number(std::max(lines(), 100)) + QLatin1Char('0');
    setMarginWidth(0, digits);
}

} // namespace slonisko
