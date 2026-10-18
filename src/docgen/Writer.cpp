// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Writer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QSaveFile>
#include <QThreadPool>

#include <map>

namespace slonisko::docgen {

QString formatName(Format format)
{
    switch (format) {
    case Format::Html:
        return QStringLiteral("HTML");
    case Format::Markdown:
        return QStringLiteral("Markdown");
    case Format::AsciiDoc:
        return QStringLiteral("AsciiDoc");
    case Format::Antora:
        return QStringLiteral("AsciiDoc for Antora");
    case Format::Sphinx:
        return QStringLiteral("reStructuredText for Sphinx");
    }
    return {};
}

QString formatKey(Format format)
{
    switch (format) {
    case Format::Html:
        return QStringLiteral("html");
    case Format::Markdown:
        return QStringLiteral("markdown");
    case Format::AsciiDoc:
        return QStringLiteral("asciidoc");
    case Format::Antora:
        return QStringLiteral("antora");
    case Format::Sphinx:
        return QStringLiteral("sphinx");
    }
    return {};
}

std::optional<Format> formatFromKey(const QString &key)
{
    const QString lower = key.toLower();
    if (lower == QLatin1String("md"))
        return Format::Markdown;
    if (lower == QLatin1String("adoc"))
        return Format::AsciiDoc;
    if (lower == QLatin1String("rst"))
        return Format::Sphinx;
    for (const Format format : allFormats()) {
        if (formatKey(format) == lower)
            return format;
    }
    return std::nullopt;
}

std::vector<Format> allFormats()
{
    return {Format::Html, Format::Markdown, Format::AsciiDoc, Format::Antora, Format::Sphinx};
}

namespace {

QString extension(Format format)
{
    switch (format) {
    case Format::Html:
        return QStringLiteral(".html");
    case Format::Markdown:
        return QStringLiteral(".md");
    case Format::Sphinx:
        return QStringLiteral(".rst");
    default:
        return QStringLiteral(".adoc");
    }
}

// Where pages go inside the output directory: an Antora component keeps
// them in its module.
QString pagesRoot(Format format)
{
    return format == Format::Antora ? QStringLiteral("modules/ROOT/pages/") : QString();
}

QString relativePath(const QString &from, const QString &to)
{
    QStringList fromDirectories = from.split(QLatin1Char('/'));
    fromDirectories.removeLast();
    const QStringList toParts = to.split(QLatin1Char('/'));
    qsizetype common = 0;
    while (common < fromDirectories.size() && common < toParts.size() - 1
           && fromDirectories[common] == toParts[common])
        ++common;
    QString path;
    for (qsizetype i = common; i < fromDirectories.size(); ++i)
        path += QStringLiteral("../");
    return path + toParts.mid(common).join(QLatin1Char('/'));
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Could not create the directory for %1").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

// Runs Graphviz over every diagram of the HTML pages, several at a time,
// as a big schema has a diagram for nearly every table.
std::map<std::pair<int, int>, QString> drawDiagrams(const Site &site, const QString &dot,
                                                    QStringList *warnings)
{
    std::map<std::pair<int, int>, QString> svgs;
    QMutex mutex;
    QThreadPool pool;
    for (int p = 0; p < int(site.pages.size()); ++p) {
        const Page &page = site.pages[p];
        for (int b = 0; b < int(page.blocks.size()); ++b) {
            if (page.blocks[b].kind != Block::Kind::Diagram)
                continue;
            const QByteArray source = linkedDot(page.blocks[b], page.id).toUtf8();
            pool.start([&, p, b, source] {
                QProcess process;
                process.start(dot, {QStringLiteral("-Tsvg")});
                process.write(source);
                process.closeWriteChannel();
                const bool done = process.waitForFinished(120'000);
                const QByteArray out = process.readAllStandardOutput();
                QMutexLocker lock(&mutex);
                if (!done || process.exitStatus() != QProcess::NormalExit
                    || process.exitCode() != 0) {
                    warnings->append(
                        QStringLiteral("Graphviz could not draw the diagram of %1: %2")
                            .arg(site.pages[p].title,
                                 QString::fromUtf8(process.readAllStandardError()).trimmed()));
                    return;
                }
                // The SVG goes into the page, without its XML prologue.
                const qsizetype start = out.indexOf("<svg");
                if (start >= 0)
                    svgs[{p, b}] = QString::fromUtf8(out.mid(start));
            });
        }
    }
    pool.waitForDone();
    return svgs;
}

} // namespace

QString pageFile(const QString &id, Format format)
{
    return pagesRoot(format) + id + extension(format);
}

QString linkTo(const QString &fromPage, const Target &target, Format format)
{
    if (target.isNull())
        return {};
    const QString fragment = target.anchor.isEmpty() ? QString() : QLatin1Char('#') + target.anchor;
    if (target.page == fromPage && !fragment.isEmpty())
        return fragment;
    // Antora resolves a page id from the module's pages, not from the page
    // the link is on.
    if (format == Format::Antora)
        return target.page + extension(format) + fragment;
    if (format == Format::Sphinx)
        return QLatin1Char('/') + target.page + fragment;
    return relativePath(fromPage, target.page) + extension(format) + fragment;
}

QString linkedDot(const Block &diagram, const QString &fromPage)
{
    QString dot = diagram.dot;
    const qsizetype end = dot.lastIndexOf(QLatin1Char('}'));
    if (end < 0)
        return dot;
    // A node given again outside its cluster stays in the cluster and only
    // gains the attribute.
    QString links;
    for (const auto &[node, target] : diagram.nodeLinks)
        links += QStringLiteral("    %1 [URL=\"%2\", target=\"_top\"];\n")
                     .arg(node, linkTo(fromPage, target, Format::Html));
    dot.insert(end, links);
    return dot;
}

WriteResult writeSite(const Site &site, const WriteOptions &options)
{
    WriteResult result;
    const QDir root(options.directory);
    if (options.directory.isEmpty() || !QDir().mkpath(root.absolutePath())) {
        result.error
            = QStringLiteral("Could not create the output directory %1").arg(options.directory);
        return result;
    }
    const auto write = [&](const QString &relative, const QByteArray &data) {
        return writeFile(root.filePath(relative), data, &result.error);
    };

    std::map<std::pair<int, int>, QString> svgs;
    if (options.format == Format::Html && !options.dot.isEmpty())
        svgs = drawDiagrams(site, options.dot, &result.warnings);

    const int total = int(site.pages.size());
    for (int p = 0; p < total; ++p) {
        const Page &page = site.pages[p];
        QString text;
        switch (options.format) {
        case Format::Html: {
            QHash<int, QString> drawn;
            for (int b = 0; b < int(page.blocks.size()); ++b) {
                const auto it = svgs.find({p, b});
                if (it != svgs.end())
                    drawn.insert(b, it->second);
            }
            text = renderHtml(site, page, drawn);
            break;
        }
        case Format::Markdown:
            text = renderMarkdown(site, page);
            break;
        case Format::AsciiDoc:
        case Format::Antora:
            text = renderAsciiDoc(site, page, options.format);
            break;
        case Format::Sphinx:
            text = renderSphinx(site, page);
            break;
        }
        if (!write(pageFile(page.id, options.format), text.toUtf8()))
            return result;
        ++result.pages;
        if (options.progress)
            options.progress(p + 1, total);
    }

    switch (options.format) {
    case Format::Html: {
        QByteArray css = defaultStylesheet().toUtf8();
        if (!options.stylesheet.isEmpty()) {
            QFile file(options.stylesheet);
            if (!file.open(QIODevice::ReadOnly)) {
                result.error = QStringLiteral("Could not read the stylesheet %1: %2")
                                   .arg(options.stylesheet, file.errorString());
                return result;
            }
            css = file.readAll();
        }
        if (!write(QStringLiteral("style.css"), css))
            return result;
        result.entryPoint = root.filePath(pageFile(site.pages.front().id, options.format));
        break;
    }
    case Format::Antora: {
        QString yml
            = QStringLiteral("name: %1\ntitle: %2\nversion: %3\nnav:\n- modules/ROOT/nav.adoc\n")
                  .arg(options.antoraName,
                       // Quoted, as a title can hold a colon.
                       QLatin1Char('"')
                           + QString(site.title).replace(QLatin1Char('"'), QLatin1String("\\\""))
                           + QLatin1Char('"'),
                       options.antoraVersion.isEmpty() ? QStringLiteral("~")
                                                       : options.antoraVersion);
        if (!write(QStringLiteral("antora.yml"), yml.toUtf8())
            || !write(QStringLiteral("modules/ROOT/nav.adoc"), antoraNavigation(site).toUtf8()))
            return result;
        result.entryPoint = root.filePath(QStringLiteral("antora.yml"));
        break;
    }
    case Format::Sphinx:
        if (!write(QStringLiteral("conf.py"), sphinxConfiguration(site).toUtf8()))
            return result;
        result.entryPoint = root.filePath(QStringLiteral("conf.py"));
        break;
    default:
        result.entryPoint = root.filePath(pageFile(site.pages.front().id, options.format));
    }
    return result;
}

} // namespace slonisko::docgen
