// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Writer.h"

namespace slonisko::docgen {

namespace {

// Text that Markdown takes as it is: its punctuation escaped, and inside a
// table no line breaks, which would end the row.
QString escaped(const QString &text, bool inTable)
{
    static const QString special = QStringLiteral("\\`*_[]<>|~#");
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (special.contains(c))
            out += QLatin1Char('\\');
        out += c;
    }
    if (inTable)
        out.replace(QLatin1Char('\n'), QLatin1String("<br>"));
    return out;
}

// A code span needs a longer run of backticks around it than any inside.
QString codeSpan(const QString &text, bool inTable)
{
    int longest = 0;
    int run = 0;
    for (const QChar c : text) {
        run = c == QLatin1Char('`') ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const QString fence(longest + 1, QLatin1Char('`'));
    QString body = text;
    if (inTable) {
        // GitHub splits table cells before it looks for code spans.
        body.replace(QLatin1Char('|'), QLatin1String("\\|"));
        body.replace(QLatin1Char('\n'), QLatin1Char(' '));
    }
    const bool pad = body.startsWith(QLatin1Char('`')) || body.endsWith(QLatin1Char('`'));
    return fence + (pad ? QStringLiteral(" ") : QString()) + body
        + (pad ? QStringLiteral(" ") : QString()) + fence;
}

QString fenced(const QString &language, const QString &text)
{
    const QString fence
        = text.contains(QLatin1String("```")) ? QStringLiteral("~~~~") : QStringLiteral("```");
    return fence + language + QLatin1Char('\n') + text + QLatin1Char('\n') + fence
        + QLatin1Char('\n');
}

class Renderer
{
public:
    explicit Renderer(const Page &page) : m_page(page) { }

    QString inlineText(const Inline &text, bool inTable) const
    {
        QString out;
        for (const Span &span : text) {
            QString part;
            switch (span.style) {
            case Span::Style::Code:
                part = codeSpan(span.text, inTable);
                break;
            case Span::Style::Strong:
                part = QStringLiteral("**") + escaped(span.text, inTable) + QStringLiteral("**");
                break;
            case Span::Style::Emphasis:
                part = QLatin1Char('*') + escaped(span.text, inTable) + QLatin1Char('*');
                break;
            case Span::Style::Plain:
                part = escaped(span.text, inTable);
                break;
            }
            if (!span.link.isNull() && !span.text.isEmpty())
                part = QLatin1Char('[') + part + QStringLiteral("](")
                    + linkTo(m_page.id, span.link, Format::Markdown) + QLatin1Char(')');
            out += part;
        }
        return out;
    }

    QString row(const std::vector<Inline> &cells, const QString &anchor = {}) const
    {
        QString out = QStringLiteral("|");
        for (std::size_t i = 0; i < cells.size(); ++i) {
            QString cell = inlineText(cells[i], true);
            if (i == 0 && !anchor.isEmpty())
                cell.prepend(QStringLiteral("<a id=\"%1\"></a>").arg(anchor));
            out += QLatin1Char(' ') + cell + QStringLiteral(" |");
        }
        return out + QLatin1Char('\n');
    }

    QString block(const Block &block) const
    {
        switch (block.kind) {
        case Block::Kind::Heading: {
            QString out;
            if (!block.anchor.isEmpty())
                out += QStringLiteral("<a id=\"%1\"></a>\n\n").arg(block.anchor);
            return out + QString(block.level, QLatin1Char('#')) + QLatin1Char(' ')
                + inlineText(block.text, false) + QLatin1Char('\n');
        }
        case Block::Kind::Paragraph:
            return inlineText(block.text, false) + QLatin1Char('\n');
        case Block::Kind::Code:
            return fenced(QStringLiteral("sql"), block.code);
        case Block::Kind::Diagram:
            return fenced(QStringLiteral("mermaid"), block.mermaid.trimmed());
        case Block::Kind::List: {
            QString out;
            for (const auto &item : block.rows)
                out += QStringLiteral("- ") + inlineText(item.front(), false) + QLatin1Char('\n');
            return out;
        }
        case Block::Kind::Properties:
        case Block::Kind::Table: {
            if (block.rows.empty())
                return {};
            std::vector<Inline> header = block.header;
            if (block.kind == Block::Kind::Properties)
                header = {plain(QStringLiteral("Property")), plain(QStringLiteral("Value"))};
            QString out = row(header);
            out += QStringLiteral("|");
            for (std::size_t i = 0; i < header.size(); ++i)
                out += QStringLiteral(" --- |");
            out += QLatin1Char('\n');
            for (std::size_t r = 0; r < block.rows.size(); ++r)
                out += row(block.rows[r], block.rowAnchors.value(qsizetype(r)));
            return out;
        }
        }
        return {};
    }

private:
    const Page &m_page;
};

} // namespace

QString renderMarkdown(const Site &site, const Page &page)
{
    const Renderer renderer(page);
    QString out;
    if (!page.trail.empty()) {
        QStringList crumbs;
        for (const auto &[title, target] : page.trail)
            crumbs << renderer.inlineText(link(title, target), false);
        out += crumbs.join(QStringLiteral(" › ")) + QStringLiteral("\n\n");
    }
    out += QStringLiteral("# ") + renderer.inlineText(plain(page.title), false) + QLatin1Char('\n');
    if (!page.kind.isEmpty())
        out += QStringLiteral("\n*") + escaped(page.kind, false) + QStringLiteral("*\n");
    for (const Block &block : page.blocks) {
        const QString text = renderer.block(block);
        if (!text.isEmpty())
            out += QLatin1Char('\n') + text;
    }
    out += QStringLiteral("\n---\n\n*Generated by %1 on %2.*\n")
               .arg(escaped(site.generator, false), escaped(site.generatedAt, false));
    return out;
}

} // namespace slonisko::docgen
