// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Document.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace slonisko::docgen {

enum class Format {
    Html, // A static website.
    Markdown, // GitHub-flavoured, diagrams as Mermaid.
    AsciiDoc, // Plain AsciiDoc for Asciidoctor, diagrams as Graphviz blocks.
    Antora, // AsciiDoc laid out as an Antora component, with its navigation.
    Sphinx, // reStructuredText with conf.py, ready for sphinx-build.
};

QString formatName(Format format);
// "html", "markdown", "asciidoc", "antora", "sphinx": what the command line
// takes.
QString formatKey(Format format);
std::optional<Format> formatFromKey(const QString &key);
std::vector<Format> allFormats();

struct WriteOptions
{
    Format format = Format::Html;
    QString directory;
    // The Graphviz program drawing HTML diagrams; empty leaves them as their
    // DOT source.
    QString dot;
    QString stylesheet; // A CSS file to use instead of the built-in one.
    QString antoraName = QStringLiteral("schema"); // The component's name.
    QString antoraVersion; // Empty: unversioned.
    // Called for each page written, with how many there are; may be empty.
    std::function<void(int done, int total)> progress;
};

struct WriteResult
{
    QString error; // Empty on success.
    QString entryPoint; // The file to open first.
    int pages = 0;
    QStringList warnings;

    bool ok() const { return error.isEmpty(); }
};

// Writes every page into the directory, creating it and what is under it,
// replacing files of the same names and leaving anything else alone.
WriteResult writeSite(const Site &site, const WriteOptions &options);

// Where a page goes, relative to the output directory.
QString pageFile(const QString &id, Format format);

// The link from one page to a target, as the format spells it: a relative
// path, for Antora a page id within the module, for Sphinx a document name
// from the project's root.
QString linkTo(const QString &fromPage, const Target &target, Format format);

// One page's text. For HTML, svgs are drawn diagrams by their index among
// the page's blocks; the rest show their source.
QString renderHtml(const Site &site, const Page &page, const QHash<int, QString> &svgs = {});
QString renderMarkdown(const Site &site, const Page &page);
QString renderAsciiDoc(const Site &site, const Page &page, Format format);
QString antoraNavigation(const Site &site);
QString renderSphinx(const Site &site, const Page &page);
QString sphinxConfiguration(const Site &site);
QString defaultStylesheet();

// The DOT source with a link on every table that has a page, relative to
// the page the diagram is on.
QString linkedDot(const Block &diagram, const QString &fromPage);

} // namespace slonisko::docgen
