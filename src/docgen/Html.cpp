// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Writer.h"

#include "sql/Lexer.h"

namespace slonisko::docgen {

namespace {

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

// SQL with its keywords, strings and comments marked, by the same lexer
// the editor highlights with.
QString highlighted(const QString &sql)
{
    const QByteArray utf8 = sql.toUtf8();
    QString out;
    for (const sql::Token &token : sql::tokenize(utf8)) {
        const QString text = escaped(QString::fromUtf8(utf8.mid(token.offset, token.length)));
        const char *css = nullptr;
        switch (token.kind) {
        case sql::TokenKind::Comment:
            css = "c";
            break;
        case sql::TokenKind::Keyword:
            css = "k";
            break;
        case sql::TokenKind::String:
        case sql::TokenKind::DollarString:
        case sql::TokenKind::DollarDelimiter:
            css = "s";
            break;
        case sql::TokenKind::Number:
            css = "n";
            break;
        default:
            break;
        }
        out += css ? QStringLiteral("<span class=\"%1\">%2</span>").arg(QLatin1String(css), text)
                   : text;
    }
    return out;
}

class Renderer
{
public:
    Renderer(const Page &page, const QHash<int, QString> &svgs) : m_page(page), m_svgs(svgs) { }

    QString inlineText(const Inline &text) const
    {
        QString out;
        for (const Span &span : text) {
            QString part = escaped(span.text);
            switch (span.style) {
            case Span::Style::Code:
                part = QStringLiteral("<code>") + part + QStringLiteral("</code>");
                break;
            case Span::Style::Strong:
                part = QStringLiteral("<strong>") + part + QStringLiteral("</strong>");
                break;
            case Span::Style::Emphasis:
                part = QStringLiteral("<em>") + part + QStringLiteral("</em>");
                break;
            case Span::Style::Plain:
                break;
            }
            if (!span.link.isNull())
                part = QStringLiteral("<a href=\"%1\">%2</a>")
                           .arg(escaped(linkTo(m_page.id, span.link, Format::Html)), part);
            out += part;
        }
        return out;
    }

    QString block(const Block &block, int index) const
    {
        const QString id = block.anchor.isEmpty()
            ? QString()
            : QStringLiteral(" id=\"%1\"").arg(escaped(block.anchor));
        switch (block.kind) {
        case Block::Kind::Heading:
            return QStringLiteral("<h%1%2>%3</h%1>\n")
                .arg(block.level)
                .arg(id, inlineText(block.text));
        case Block::Kind::Paragraph:
            return QStringLiteral("<p>%1</p>\n").arg(inlineText(block.text));
        case Block::Kind::Code:
            return QStringLiteral("<pre class=\"sql\"><code>%1</code></pre>\n")
                .arg(highlighted(block.code));
        case Block::Kind::Diagram: {
            const auto it = m_svgs.constFind(index);
            if (it != m_svgs.cend())
                return QStringLiteral("<div class=\"diagram\">\n%1</div>\n").arg(*it);
            return QStringLiteral("<details class=\"diagram-source\"><summary>Diagram as Graphviz "
                                  "source; Graphviz was not there to draw it.</summary>\n"
                                  "<pre>%1</pre></details>\n")
                .arg(escaped(block.dot));
        }
        case Block::Kind::List: {
            QString out = QStringLiteral("<ul>\n");
            for (const auto &item : block.rows)
                out += QStringLiteral("<li>%1</li>\n").arg(inlineText(item.front()));
            return out + QStringLiteral("</ul>\n");
        }
        case Block::Kind::Properties: {
            if (block.rows.empty())
                return {};
            QString out = QStringLiteral("<table class=\"properties\">\n");
            for (const auto &row : block.rows)
                out += QStringLiteral("<tr><th>%1</th><td>%2</td></tr>\n")
                           .arg(inlineText(row[0]), inlineText(row[1]));
            return out + QStringLiteral("</table>\n");
        }
        case Block::Kind::Table: {
            if (block.rows.empty())
                return {};
            QString out = QStringLiteral("<div class=\"scroll\"><table%1>\n<thead><tr>").arg(id);
            for (const Inline &cell : block.header)
                out += QStringLiteral("<th>%1</th>").arg(inlineText(cell));
            out += QStringLiteral("</tr></thead>\n<tbody>\n");
            for (std::size_t r = 0; r < block.rows.size(); ++r) {
                const QString anchor = block.rowAnchors.value(qsizetype(r));
                out += anchor.isEmpty() ? QStringLiteral("<tr>")
                                        : QStringLiteral("<tr id=\"%1\">").arg(escaped(anchor));
                for (const Inline &cell : block.rows[r])
                    out += QStringLiteral("<td>%1</td>").arg(inlineText(cell));
                out += QStringLiteral("</tr>\n");
            }
            return out + QStringLiteral("</tbody>\n</table></div>\n");
        }
        }
        return {};
    }

private:
    const Page &m_page;
    const QHash<int, QString> &m_svgs;
};

} // namespace

QString renderHtml(const Site &site, const Page &page, const QHash<int, QString> &svgs)
{
    const Renderer renderer(page, svgs);
    const auto href
        = [&page](const QString &id) { return escaped(linkTo(page.id, {id, {}}, Format::Html)); };
    const QString css = QStringLiteral("../").repeated(page.id.count(QLatin1Char('/')))
        + QStringLiteral("style.css");

    QString out = QStringLiteral("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                                 "<meta charset=\"utf-8\">\n"
                                 "<meta name=\"viewport\" content=\"width=device-width, "
                                 "initial-scale=1\">\n"
                                 "<meta name=\"generator\" content=\"%1\">\n"
                                 "<title>%2 – %3</title>\n"
                                 "<link rel=\"stylesheet\" href=\"%4\">\n</head>\n<body>\n")
                      .arg(escaped(site.generator), escaped(page.title), escaped(site.title), css);

    out += QStringLiteral("<header><a class=\"site\" href=\"%1\">%2</a><nav>")
               .arg(href(site.pages.front().id), escaped(site.title));
    if (site.page(QStringLiteral("names")))
        out += QStringLiteral("<a href=\"%1\">Index</a>").arg(href(QStringLiteral("names")));
    out += QStringLiteral("</nav></header>\n<main>\n");

    if (!page.trail.empty()) {
        QStringList crumbs;
        for (const auto &[title, target] : page.trail)
            crumbs << renderer.inlineText(link(title, target));
        out += QStringLiteral("<p class=\"trail\">%1</p>\n")
                   .arg(crumbs.join(QStringLiteral(" › ")));
    }
    out += QStringLiteral("<h1>%1</h1>\n").arg(escaped(page.title));
    if (!page.kind.isEmpty())
        out += QStringLiteral("<p class=\"kind\">%1</p>\n").arg(escaped(page.kind));
    for (int i = 0; i < int(page.blocks.size()); ++i)
        out += renderer.block(page.blocks[i], i);
    out += QStringLiteral("</main>\n<footer>Generated by %1 on %2.</footer>\n</body>\n</html>\n")
               .arg(escaped(site.generator), escaped(site.generatedAt));
    return out;
}

QString defaultStylesheet()
{
    return QStringLiteral(R"css(/* slonisko-docgen */
:root {
    --bg: #ffffff;
    --fg: #1f2328;
    --muted: #59636e;
    --line: #d1d9e0;
    --stripe: #f6f8fa;
    --accent: #0b5cad;
    --header: #1f3a5f;
    --code-bg: #f6f8fa;
    --keyword: #8250df;
    --string: #0a3069;
    --comment: #6e7781;
    --number: #953800;
}
@media (prefers-color-scheme: dark) {
    :root {
        --bg: #0d1117;
        --fg: #e6edf3;
        --muted: #9198a1;
        --line: #3d444d;
        --stripe: #151b23;
        --accent: #4493f8;
        --header: #10243d;
        --code-bg: #151b23;
        --keyword: #d2a8ff;
        --string: #a5d6ff;
        --comment: #9198a1;
        --number: #ffa657;
    }
}
* { box-sizing: border-box; }
body {
    margin: 0;
    background: var(--bg);
    color: var(--fg);
    font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif;
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
header {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 1.5em;
    padding: 0.6em 16px;
    background: var(--header);
}
header a { color: #ffffff; }
header .site { font-weight: 600; font-size: 1.1em; }
header nav { display: flex; gap: 1em; }
main { max-width: 1200px; margin: 0 auto; padding: 0 16px 2em; }
.trail { color: var(--muted); margin: 1em 0 0; }
h1 { margin: 0.3em 0 0; font-size: 1.8em; overflow-wrap: anywhere; }
.kind { color: var(--muted); margin: 0 0 1em; font-style: italic; }
h2 { margin-top: 1.8em; border-bottom: 1px solid var(--line); padding-bottom: 0.2em; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; margin: 0.5em 0; }
th, td { border: 1px solid var(--line); padding: 0.3em 0.6em; text-align: left; vertical-align: top; }
thead th { background: var(--stripe); }
tbody tr:nth-child(even) { background: var(--stripe); }
tr:target { outline: 2px solid var(--accent); }
table.properties th { background: var(--stripe); font-weight: 600; white-space: nowrap; }
code, pre { font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; font-size: 0.92em; }
td code { white-space: pre-wrap; }
pre {
    background: var(--code-bg);
    border: 1px solid var(--line);
    padding: 0.8em;
    overflow-x: auto;
}
pre .k { color: var(--keyword); }
pre .s { color: var(--string); }
pre .c { color: var(--comment); font-style: italic; }
pre .n { color: var(--number); }
/* Graphviz draws on white, in its own colours. */
.diagram { overflow-x: auto; background: #ffffff; border: 1px solid var(--line); padding: 0.5em; }
.diagram svg { max-width: 100%; height: auto; }
footer { color: var(--muted); font-size: 0.85em; text-align: center; padding: 2em 16px; }
)css");
}

} // namespace slonisko::docgen
