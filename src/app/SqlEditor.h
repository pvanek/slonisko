// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Qsci.h"
#include "catalog/Completion.h"
#include "catalog/Semantic.h"

#include <QFutureWatcher>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>

namespace slonisko {

class CompletionPopup;
class SqlLexer;

// A piece of the script to run: one statement, a psql command to skip, or
// a COPY with its inline data.
struct ScriptPiece
{
    QByteArray sql;
    std::optional<QByteArray> copyData;
    qsizetype offset = 0; // Of sql in the document, in bytes.
    bool psqlCommand = false;
};

// The SQL text editor: QScintilla with PostgreSQL highlighting, line
// numbers, and a mark on the statement Run would execute. All positions are
// UTF-8 byte offsets, as in Scintilla.
class SqlEditor : public QsciScintilla
{
    Q_OBJECT

public:
    explicit SqlEditor(QWidget *parent = nullptr);

    QByteArray utf8Text() const;
    qsizetype cursorPosition() const;
    void setCursorPosition(qsizetype pos);
    void selectRange(qsizetype from, qsizetype to);

    // What Run executes: the selected statements, or the current one.
    std::vector<ScriptPiece> piecesToRun() const;
    // The current statement's range in bytes, if the cursor is in one.
    std::optional<std::pair<qsizetype, qsizetype>> currentStatementRange() const;

    // Underlines the token at a byte position as an error, until the text
    // changes or clearErrors() is called.
    void markError(qsizetype pos, const QString &message = {});
    void clearErrors();
    bool hasErrorAt(qsizetype pos) const;

    // Where completion gets catalog data from; it may return null while the
    // data is still loading, and then only keywords are offered.
    using SnapshotProvider = std::function<catalog::SnapshotPtr()>;
    void setSnapshotProvider(SnapshotProvider provider) { m_snapshot = std::move(provider); }
    // Suggests completions for the word at the cursor, off the UI thread.
    // Explicit requests (Ctrl+Space) show suggestions for an empty word too.
    void complete(bool explicitRequest = true);
    CompletionPopup *completionPopup() const { return m_popup; }
    // The statement around a byte position, as completion sees it: the text
    // between the previous and the next statement, even if unterminated.
    static std::pair<qsizetype, qsizetype> statementBounds(const QByteArray &text, qsizetype pos);

    static constexpr int StatementIndicator = 8;
    static constexpr int ErrorIndicator = 9;
    // Semantic highlighting: one indicator per SemanticSpan::Kind, from here
    // on, in that order. Later ones are drawn over earlier ones.
    static constexpr int FirstSemanticIndicator = 10;
    static int indicatorFor(catalog::SemanticSpan::Kind kind);

    // Analyzes the visible statements again, e.g. after the catalog changed.
    void refreshSemantics();
    const std::vector<catalog::SemanticSpan> &semanticSpans() const { return m_semantic; }
    // What hovering over a position explains: a name's detail or an error.
    QString explanationAt(qsizetype pos) const;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void analyzeVisible();
    void applySemantics();
    void setupSemanticIndicators();
    void showCompletion();
    void applyCompletion(const catalog::CompletionItem &item);

    // SendScintilla() with byte offsets, without its overload ambiguities.
    long send(unsigned int message, qsizetype wParam = 0, qsizetype lParam = 0) const
    {
        return SendScintilla(message, static_cast<unsigned long>(wParam),
                             static_cast<long>(lParam));
    }
    void updateStatementMark();
    void updateMarginWidth();
    std::vector<ScriptPiece> pieces(qsizetype from, qsizetype to) const;

    SqlLexer *m_lexer = nullptr;
    QTimer m_statementTimer;

    SnapshotProvider m_snapshot;
    CompletionPopup *m_popup = nullptr;
    QTimer m_completionTimer;
    QFutureWatcher<catalog::Completion> m_completionWatcher;
    quint64 m_revision = 0; // Counts edits, to drop stale completions.
    quint64 m_requestRevision = 0;
    qsizetype m_requestCursor = 0;
    qsizetype m_statementStart = 0; // Of the statement the completion is for.
    bool m_explicitRequest = false;
    catalog::Completion m_completion; // The one on show.

    QTimer m_semanticTimer;
    QFutureWatcher<std::vector<catalog::SemanticSpan>> m_semanticWatcher;
    quint64 m_semanticRevision = 0;
    std::vector<catalog::SemanticSpan> m_semantic;
    std::vector<std::pair<std::pair<qsizetype, qsizetype>, QString>> m_errors; // Range, message.
};

} // namespace slonisko
