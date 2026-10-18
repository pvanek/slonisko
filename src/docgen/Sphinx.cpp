// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Writer.h"

namespace slonisko::docgen {

namespace {

// Text exactly as it is: reStructuredText's inline markup escaped, and at
// the start of a paragraph or cell whatever would begin a list.
QString escaped(const QString &text)
{
    static const QString special = QStringLiteral("\\*`_|[]<>");
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (special.contains(c))
            out += QLatin1Char('\\');
        out += c == QLatin1Char('\n') ? QLatin1Char(' ') : c;
    }
    return out;
}

QString escapedStart(const QString &text)
{
    QString out = escaped(text);
    static const QString listStart = QStringLiteral("-+#.:");
    if (!out.isEmpty() && (listStart.contains(out.front()) || out.front().isDigit()))
        out.prepend(QLatin1Char('\\'));
    return out;
}

// Labels are one namespace for the whole project, so each carries its page.
QString label(const Target &target)
{
    QString page = target.page;
    page.replace(QLatin1Char('/'), QLatin1Char('-'));
    return (page + QStringLiteral("--") + target.anchor).toLower();
}

// Overline and underline at least as wide as the title, wide characters
// counting twice.
QString rule(const QString &title, QChar c)
{
    qsizetype width = 0;
    for (const QChar ch : title)
        width += ch.unicode() >= 0x1100 ? 2 : 1;
    return QString(std::max<qsizetype>(width, 3), c);
}

QString indented(const QString &text, int spaces)
{
    const QString indent(spaces, QLatin1Char(' '));
    QString out;
    for (const QString &line : text.split(QLatin1Char('\n')))
        out += (line.isEmpty() ? QString() : indent + line) + QLatin1Char('\n');
    return out;
}

class Renderer
{
public:
    explicit Renderer(const Page &page) : m_page(page) { }

    QString span(const Span &span) const
    {
        const QString text = span.text.simplified();
        if (!span.link.isNull()) {
            // Inside a role the title is plain: only < and ` need escaping.
            QString title = text;
            title.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
            title.replace(QLatin1Char('<'), QLatin1String("\\<"));
            title.replace(QLatin1Char('`'), QLatin1String("\\`"));
            if (span.link.anchor.isEmpty())
                return QStringLiteral(":doc:`%1 </%2>`").arg(title, span.link.page);
            return QStringLiteral(":ref:`%1 <%2>`").arg(title, label(span.link));
        }
        switch (span.style) {
        case Span::Style::Code:
            if (!text.contains(QLatin1String("``")))
                return QStringLiteral("``") + text + QStringLiteral("``");
            return escaped(text);
        case Span::Style::Strong:
            return QStringLiteral("**") + escaped(text) + QStringLiteral("**");
        case Span::Style::Emphasis:
            return QLatin1Char('*') + escaped(text) + QLatin1Char('*');
        case Span::Style::Plain:
            break;
        }
        return escaped(span.text);
    }

    QString inlineText(const Inline &text) const
    {
        QString out;
        bool previousMarkup = false;
        for (const Span &s : text) {
            if (s.text.trimmed().isEmpty() && s.link.isNull()) {
                out += escaped(s.text);
                previousMarkup = false;
                continue;
            }
            const bool markup = !s.link.isNull() || s.style != Span::Style::Plain;
            QString part = span(s);
            // Inline markup has to stand apart from the words around it;
            // an escaped space does that without showing.
            if (markup && !out.isEmpty() && !out.back().isSpace())
                out += QStringLiteral("\\ ");
            else if (previousMarkup && !part.isEmpty() && !part.front().isSpace()
                     && !part.front().isPunct())
                out += QStringLiteral("\\ ");
            out += part;
            previousMarkup = markup;
        }
        // Plain text beginning a paragraph or a cell could pass for a list.
        static const QString listStart = QStringLiteral("-+#.:");
        if (!out.isEmpty() && text.front().link.isNull() && text.front().style == Span::Style::Plain
            && (listStart.contains(out.front()) || out.front().isDigit()))
            out.prepend(QLatin1Char('\\'));
        return out;
    }

    QString table(const std::vector<Inline> &header, const std::vector<std::vector<Inline>> &rows,
                  const QStringList &anchors, bool stub) const
    {
        QString out = QStringLiteral(".. list-table::\n");
        if (!header.empty())
            out += QStringLiteral("   :header-rows: 1\n");
        if (stub)
            out += QStringLiteral("   :stub-columns: 1\n");
        out += QLatin1Char('\n');
        const auto row = [&](const std::vector<Inline> &cells, const QString &anchor) {
            for (std::size_t c = 0; c < cells.size(); ++c) {
                out += c == 0 ? QStringLiteral("   * - ") : QStringLiteral("     - ");
                // A label in the cell, before what it labels.
                if (c == 0 && !anchor.isEmpty())
                    out += QStringLiteral(".. _%1:\n\n       ").arg(label({m_page.id, anchor}));
                out += inlineText(cells[c]) + QLatin1Char('\n');
            }
        };
        if (!header.empty())
            row(header, {});
        for (std::size_t r = 0; r < rows.size(); ++r)
            row(rows[r], anchors.value(qsizetype(r)));
        return out;
    }

    QString block(const Block &block) const
    {
        switch (block.kind) {
        case Block::Kind::Heading: {
            QString out;
            if (!block.anchor.isEmpty())
                out += QStringLiteral(".. _%1:\n\n").arg(label({m_page.id, block.anchor}));
            const QString title = inlineText(block.text);
            return out + title + QLatin1Char('\n')
                + rule(title, block.level <= 2 ? QLatin1Char('-') : QLatin1Char('~'))
                + QLatin1Char('\n');
        }
        case Block::Kind::Paragraph:
            return inlineText(block.text) + QLatin1Char('\n');
        case Block::Kind::Code:
            return QStringLiteral(".. code-block:: postgresql\n\n") + indented(block.code, 3);
        case Block::Kind::Diagram:
            // Sphinx's own extension; links in the SVG are kept relative to
            // the page, as they are for the HTML format.
            return QStringLiteral(".. graphviz::\n\n")
                + indented(linkedDot(block, m_page.id).trimmed(), 3);
        case Block::Kind::List: {
            QString out;
            for (const auto &item : block.rows)
                out += QStringLiteral("- ") + inlineText(item.front()) + QLatin1Char('\n');
            return out;
        }
        case Block::Kind::Properties:
            if (block.rows.empty())
                return {};
            return table({}, block.rows, {}, true);
        case Block::Kind::Table:
            if (block.rows.empty())
                return {};
            return table(block.header, block.rows, block.rowAnchors, false);
        }
        return {};
    }

private:
    const Page &m_page;
};

// The pages under a page in the navigation, which Sphinx wants in a
// toctree on that page.
void childrenOf(const std::vector<NavItem> &items, const QString &page, bool below,
                QStringList &out)
{
    for (const NavItem &item : items) {
        if (below) {
            if (!item.target.isNull() && item.target.page != page)
                out << item.target.page;
            else if (item.target.isNull())
                childrenOf(item.children, page, true, out);
        } else if (item.target.page == page) {
            childrenOf(item.children, page, true, out);
        } else {
            childrenOf(item.children, page, false, out);
        }
    }
}

} // namespace

QString renderSphinx(const Site &site, const Page &page)
{
    const Renderer renderer(page);
    const QString title = escapedStart(page.title);
    const QString line = rule(title, QLatin1Char('='));
    QString out = line + QLatin1Char('\n') + title + QLatin1Char('\n') + line + QLatin1Char('\n');
    if (!page.kind.isEmpty())
        out += QStringLiteral("\n*%1*\n").arg(escaped(page.kind));
    QStringList children;
    if (page.id == site.pages.front().id) {
        // The home page's children are everything at the top but itself.
        for (const NavItem &item : site.navigation) {
            if (!item.target.isNull() && item.target.page != page.id)
                children << item.target.page;
        }
    } else {
        childrenOf(site.navigation, page.id, false, children);
    }
    // Before the first section: Sphinx puts a toctree's pages in the
    // navigation under the section the toctree is in.
    if (!children.isEmpty()) {
        out += QStringLiteral("\n.. toctree::\n   :hidden:\n\n");
        for (const QString &child : children)
            out += QStringLiteral("   /") + child + QLatin1Char('\n');
    }
    for (const Block &block : page.blocks) {
        const QString text = renderer.block(block);
        if (!text.isEmpty())
            out += QLatin1Char('\n') + text;
    }

    out += QStringLiteral("\n*Generated by %1 on %2.*\n")
               .arg(escaped(site.generator), escaped(site.generatedAt));
    return out;
}

QString sphinxConfiguration(const Site &site)
{
    const auto python = [](const QString &text) {
        QString quoted = text;
        quoted.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        quoted.replace(QLatin1Char('"'), QLatin1String("\\\""));
        quoted.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return QLatin1Char('"') + quoted + QLatin1Char('"');
    };
    return QStringLiteral("# Written by %1; build with: sphinx-build -b html . _build/html\n"
                          "\n"
                          "project = %2\n"
                          "root_doc = \"index\"\n"
                          "extensions = [\"sphinx.ext.graphviz\"]\n"
                          "# SVG, so the tables in a diagram link to their pages.\n"
                          "graphviz_output_format = \"svg\"\n"
                          "exclude_patterns = [\"_build\"]\n")
        .arg(site.generator, python(site.title));
}

} // namespace slonisko::docgen
