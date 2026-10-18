// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Writer.h"

#include <QRegularExpression>

namespace slonisko::docgen {

namespace {

// Text exactly as it is, whatever AsciiDoc would make of its punctuation:
// in a passthrough that only escapes <, > and &, when there is anything to
// keep from it.
QString literal(const QString &text)
{
    static const QRegularExpression markup(QStringLiteral("[*_`#^~+{}\\[\\]\\\\<>:'\"|]"));
    QString out = text;
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (!out.contains(markup))
        return out;
    out.replace(QLatin1Char(']'), QLatin1String("\\]"));
    return QStringLiteral("pass:c[") + out + QLatin1Char(']');
}

QString delimited(const QString &text, QChar line)
{
    // The delimiter must be longer than any line of the same character inside.
    int longest = 3;
    for (const QString &l : text.split(QLatin1Char('\n'))) {
        if (!l.isEmpty() && l.count(line) == l.size())
            longest = std::max(longest, int(l.size()));
    }
    return QString(longest + 1, line);
}

class Renderer
{
public:
    Renderer(const Page &page, Format format) : m_page(page), m_format(format) { }

    QString inlineText(const Inline &text, bool inTable) const
    {
        QString out;
        for (const Span &span : text) {
            if (span.text.isEmpty())
                continue;
            QString part = literal(span.text);
            switch (span.style) {
            case Span::Style::Code:
                part = QStringLiteral("``") + part + QStringLiteral("``");
                break;
            case Span::Style::Strong:
                part = QStringLiteral("**") + part + QStringLiteral("**");
                break;
            case Span::Style::Emphasis:
                part = QStringLiteral("__") + part + QStringLiteral("__");
                break;
            case Span::Style::Plain:
                break;
            }
            if (!span.link.isNull()) {
                const QString target = linkTo(m_page.id, span.link, m_format);
                if (target.startsWith(QLatin1Char('#')))
                    part = QStringLiteral("<<%1,%2>>").arg(target.mid(1), part);
                else
                    part = QStringLiteral("xref:%1[%2]").arg(target, part);
            }
            out += part;
        }
        // The table's own separator, even inside a passthrough.
        if (inTable)
            out.replace(QLatin1Char('|'), QLatin1String("\\|"));
        return out;
    }

    QString table(const std::vector<Inline> &header, const std::vector<std::vector<Inline>> &rows,
                  const QStringList &anchors, const QString &attributes) const
    {
        QString out = attributes + QStringLiteral("\n|===\n");
        if (!header.empty()) {
            for (const Inline &cell : header)
                out += QStringLiteral("|") + inlineText(cell, true) + QLatin1Char(' ');
            out += QStringLiteral("\n\n");
        }
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t c = 0; c < rows[r].size(); ++c) {
                QString cell = inlineText(rows[r][c], true);
                if (c == 0 && r < std::size_t(anchors.size()))
                    cell.prepend(QStringLiteral("[[%1]]").arg(anchors[qsizetype(r)]));
                out += QStringLiteral("|") + cell + QLatin1Char(' ');
            }
            out += QLatin1Char('\n');
        }
        return out + QStringLiteral("|===\n");
    }

    QString block(const Block &block) const
    {
        switch (block.kind) {
        case Block::Kind::Heading: {
            QString out;
            if (!block.anchor.isEmpty())
                out += QStringLiteral("[[%1]]\n").arg(block.anchor);
            return out + QString(block.level + 1, QLatin1Char('=')) + QLatin1Char(' ')
                + inlineText(block.text, false) + QLatin1Char('\n');
        }
        case Block::Kind::Paragraph:
            return inlineText(block.text, false) + QLatin1Char('\n');
        case Block::Kind::Code: {
            const QString fence = delimited(block.code, QLatin1Char('-'));
            return QStringLiteral("[source,sql]\n") + fence + QLatin1Char('\n') + block.code
                + QLatin1Char('\n') + fence + QLatin1Char('\n');
        }
        case Block::Kind::Diagram: {
            // asciidoctor-diagram or Kroki draws it; without either it shows
            // as the source it is.
            const QString fence = delimited(block.dot, QLatin1Char('.'));
            return QStringLiteral("[graphviz,format=svg]\n") + fence + QLatin1Char('\n')
                + block.dot.trimmed() + QLatin1Char('\n') + fence + QLatin1Char('\n');
        }
        case Block::Kind::List: {
            QString out;
            for (const auto &item : block.rows)
                out += QStringLiteral("* ") + inlineText(item.front(), false) + QLatin1Char('\n');
            return out;
        }
        case Block::Kind::Properties:
            if (block.rows.empty())
                return {};
            return table({}, block.rows, {}, QStringLiteral("[cols=\"1h,3\"]"));
        case Block::Kind::Table:
            if (block.rows.empty())
                return {};
            return table(block.header, block.rows, block.rowAnchors,
                         QStringLiteral("[%header,cols=\"%1*\"]").arg(block.header.size()));
        }
        return {};
    }

private:
    const Page &m_page;
    Format m_format;
};

void navigationItems(const std::vector<NavItem> &items, int depth, Format format, QString &out)
{
    for (const NavItem &item : items) {
        out += QString(depth, QLatin1Char('*')) + QLatin1Char(' ');
        if (item.target.isNull())
            out += literal(item.title);
        else
            out += QStringLiteral("xref:%1[%2]")
                       .arg(linkTo({}, item.target, format), literal(item.title));
        out += QLatin1Char('\n');
        navigationItems(item.children, depth + 1, format, out);
    }
}

} // namespace

QString renderAsciiDoc(const Site &site, const Page &page, Format format)
{
    const Renderer renderer(page, format);
    QString out = QStringLiteral("= ") + literal(page.title) + QLatin1Char('\n');
    // Antora puts a page's title into the navigation and its breadcrumbs
    // itself; plain AsciiDoc needs them on the page.
    out += QStringLiteral(":page-kind: %1\n").arg(page.kind);
    if (format != Format::Antora && !page.trail.empty()) {
        QStringList crumbs;
        for (const auto &[title, target] : page.trail)
            crumbs << renderer.inlineText(link(title, target), false);
        out += QLatin1Char('\n') + crumbs.join(QStringLiteral(" › ")) + QLatin1Char('\n');
    }
    if (!page.kind.isEmpty())
        out += QStringLiteral("\n[.kind]\n_%1_\n").arg(literal(page.kind));
    for (const Block &block : page.blocks) {
        const QString text = renderer.block(block);
        if (!text.isEmpty())
            out += QLatin1Char('\n') + text;
    }
    out += QStringLiteral("\n'''\n\n[.generated]\n_Generated by %1 on %2._\n")
               .arg(literal(site.generator), literal(site.generatedAt));
    return out;
}

QString antoraNavigation(const Site &site)
{
    QString out;
    navigationItems(site.navigation, 1, Format::Antora, out);
    return out;
}

} // namespace slonisko::docgen
