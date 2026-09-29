// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ErdView.h"
#include "catalog/ErdExport.h"

#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

using namespace slonisko;
using catalog::ErdEdge;
using catalog::ErdGraph;
using catalog::ErdNode;

namespace {

ErdGraph library()
{
    ErdGraph graph;
    graph.focus = 2;
    ErdNode author;
    author.oid = 1;
    author.schema = QStringLiteral("public");
    author.name = QStringLiteral("author");
    author.columns.push_back({QStringLiteral("id"), QStringLiteral("integer"), true});
    ErdNode book;
    book.oid = 2;
    book.schema = QStringLiteral("public");
    book.name = QStringLiteral("book & co");
    book.columns.push_back({QStringLiteral("id"), QStringLiteral("integer"), true});
    book.columns.push_back(
        {QStringLiteral("author_id"), QStringLiteral("integer"), false, true, true});
    book.otherColumns = 2;
    graph.nodes = {book, author};
    ErdEdge written;
    written.name = QStringLiteral("book_author_id_fkey");
    written.from = 2;
    written.to = 1;
    written.fromColumns = {QStringLiteral("author_id")};
    written.toColumns = {QStringLiteral("id")};
    written.mandatory = true;
    graph.edges = {written};
    return graph;
}

QByteArray contentsOf(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestErdView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_view = std::make_unique<ErdView>();
        m_view->resize(800, 600);
        m_view->setGraph(library());
    }
    void cleanup() { m_view.reset(); }

    void svgIsWellFormedAndKeepsTheText()
    {
        const QString path = m_dir.filePath(QStringLiteral("d.svg"));
        QString error;
        QVERIFY2(m_view->saveDiagram(path, &error), qPrintable(error));
        const QByteArray svg = contentsOf(path);

        QXmlStreamReader xml(svg);
        QStringList texts;
        int paths = 0;
        bool sized = false;
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement)
                continue;
            if (xml.name() == QLatin1String("svg"))
                sized = !xml.attributes().value(QLatin1String("viewBox")).isEmpty();
            else if (xml.name() == QLatin1String("text"))
                texts << xml.readElementText();
            else if (xml.name() == QLatin1String("path"))
                ++paths;
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QVERIFY(sized);
        // Text stays text, not outlines or a picture of it.
        QVERIFY2(texts.contains(QStringLiteral("book & co")), qPrintable(texts.join(u'|')));
        QVERIFY(texts.contains(QStringLiteral("author_id")));
        QVERIFY(texts.contains(QStringLiteral("+ 2 columns")));
        QVERIFY(paths >= 3); // Two boxes and a key, at the least.
        QVERIFY(!svg.contains("<image"));
    }

    void pngIsDrawnOnWhite()
    {
        const QString path = m_dir.filePath(QStringLiteral("d.png"));
        QString error;
        QVERIFY2(m_view->saveDiagram(path, &error), qPrintable(error));
        const QImage image(path);
        QVERIFY(!image.isNull());
        QVERIFY(image.width() > 200 && image.height() > 100);
        QCOMPARE(image.pixelColor(1, 1), QColor(Qt::white));
        // Something is drawn on it.
        bool drawn = false;
        for (int y = 0; y < image.height() && !drawn; y += 2) {
            for (int x = 0; x < image.width() && !drawn; x += 2)
                drawn = image.pixelColor(x, y) != QColor(Qt::white);
        }
        QVERIFY(drawn);
    }

    void pdfIsAPdf()
    {
        const QString path = m_dir.filePath(QStringLiteral("d.pdf"));
        QString error;
        QVERIFY2(m_view->saveDiagram(path, &error), qPrintable(error));
        const QByteArray pdf = contentsOf(path);
        QVERIFY(pdf.startsWith("%PDF-"));
        QVERIFY(pdf.trimmed().endsWith("%%EOF"));
    }

    void textFormatsAreTheGeneratorsOutput()
    {
        QString error;
        for (const char *suffix : {"dot", "gv"}) {
            const QString path = m_dir.filePath(QStringLiteral("d.") + QLatin1String(suffix));
            QVERIFY2(m_view->saveDiagram(path, &error), qPrintable(error));
            QCOMPARE(QString::fromUtf8(contentsOf(path)), catalog::erdToDot(library()));
        }
        const QString path = m_dir.filePath(QStringLiteral("d.mmd"));
        QVERIFY2(m_view->saveDiagram(path, &error), qPrintable(error));
        QCOMPARE(QString::fromUtf8(contentsOf(path)), catalog::erdToMermaid(library()));
    }

    void unknownSuffixesAndEmptyDiagramsAreRefused()
    {
        QString error;
        QVERIFY(!m_view->saveDiagram(m_dir.filePath(QStringLiteral("d.bmp")), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("d.bmp"))));

        m_view->clear();
        error.clear();
        QVERIFY(!m_view->saveDiagram(m_dir.filePath(QStringLiteral("e.svg")), &error));
        QVERIFY(!error.isEmpty());
    }

    void exportLeavesTheViewAsItWas()
    {
        const QPalette before = m_view->scene()->palette();
        for (QGraphicsItem *item : m_view->scene()->items())
            item->setSelected(true); // Only the tables can be.
        const int selected = int(m_view->scene()->selectedItems().size());
        QCOMPARE(selected, 2);
        QVERIFY(m_view->saveDiagram(m_dir.filePath(QStringLiteral("d.png"))));
        QCOMPARE(m_view->scene()->palette(), before);
        QCOMPARE(int(m_view->scene()->selectedItems().size()), selected);
    }

private:
    QTemporaryDir m_dir;
    std::unique_ptr<ErdView> m_view;
};

QTEST_MAIN(TestErdView)
#include "tst_erdview.moc"
