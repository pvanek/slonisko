// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace slonisko::docgen {

// The documentation as pages of headings, paragraphs, tables and code,
// before it is any format in particular: the pages are built once, and each
// writer only decides how they are spelled.

// Where a link goes: a page, and a place on it if not its top.
struct Target
{
    QString page; // A Page::id; empty for no link.
    QString anchor;

    bool isNull() const { return page.isEmpty(); }
};

struct Span
{
    enum class Style { Plain, Code, Strong, Emphasis };

    QString text;
    Style style = Style::Plain;
    Target link;
};

// A run of text, as a paragraph or a table cell holds it.
using Inline = std::vector<Span>;

Inline plain(const QString &text);
Inline code(const QString &text);
Inline link(const QString &text, const Target &target, Span::Style style = Span::Style::Plain);

struct Block
{
    enum class Kind {
        Heading,
        Paragraph,
        Table,
        Code, // SQL, shown as it is.
        List,
        Properties, // A two-column table of names and values, without a header.
        Diagram,
    };

    Kind kind = Kind::Paragraph;
    int level = 2; // Of a heading; the page title is level 1.
    QString anchor; // Of a heading or a table, for links to it.
    Inline text; // Of a heading or a paragraph.
    std::vector<Inline> header; // Of a table.
    std::vector<std::vector<Inline>> rows; // Of a table, a list (one cell) or properties.
    // Of a table: an anchor for each row, or none, so a link can point at a
    // column in a list of columns.
    QStringList rowAnchors;
    QString code; // Of a code block.
    // Of a diagram: the same graph for Graphviz and for Mermaid, and the
    // page each table links to, by its oid as Graphviz knows it ("t<oid>").
    QString dot;
    QString mermaid;
    std::vector<std::pair<QString, Target>> nodeLinks;
};

Block heading(const QString &text, int level = 2, const QString &anchor = {});
Block paragraph(const Inline &text);
Block codeBlock(const QString &sql);

struct Page
{
    QString id; // Unique, also its path without the extension, like "public/tables/orders".
    QString title;
    QString kind; // "table", "view", ...: shown under the title.
    // The pages above this one, for a breadcrumb trail; the first is the
    // home page.
    std::vector<std::pair<QString, Target>> trail;
    std::vector<Block> blocks;
};

// A navigation tree, for formats that have one beside the pages.
struct NavItem
{
    QString title;
    Target target;
    std::vector<NavItem> children;
};

struct Site
{
    QString title;
    QString generator; // "slonisko-docgen 0.1.2"
    QString generatedAt;
    std::vector<Page> pages; // The home page first.
    std::vector<NavItem> navigation;

    const Page *page(const QString &id) const;
};

} // namespace slonisko::docgen
