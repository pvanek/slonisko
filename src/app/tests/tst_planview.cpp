// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlanGraphView.h"
#include "PlanView.h"
#include "catalog/PlanExport.h"

#include <QClipboard>
#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QScrollArea>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QXmlStreamReader>

using namespace slonisko;
using catalog::PlanNode;

namespace {

// Hash Left Join (cost 100) over a Seq Scan (60) and a Hash (10) over an
// Index Only Scan (10).
const QByteArray Estimated = R"json([{"Plan": {
  "Node Type": "Hash Join", "Join Type": "Left", "Startup Cost": 10.0, "Total Cost": 100.0,
  "Plan Rows": 50, "Plan Width": 8, "Hash Cond": "(o.customer_id = c.id)",
  "Plans": [
    {"Node Type": "Seq Scan", "Relation Name": "orders", "Schema": "public", "Alias": "o",
     "Startup Cost": 0.0, "Total Cost": 60.0, "Plan Rows": 50, "Plan Width": 8},
    {"Node Type": "Hash", "Startup Cost": 10.0, "Total Cost": 10.0, "Plan Rows": 10,
     "Plan Width": 4, "Plans": [
       {"Node Type": "Index Only Scan", "Index Name": "customers_pkey",
        "Relation Name": "customers", "Schema": "public", "Alias": "c",
        "Startup Cost": 0.0, "Total Cost": 10.0, "Plan Rows": 10, "Plan Width": 4}]}
  ]}}])json";

QByteArray contentsOf(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The text of every <text> element of an SVG.
QStringList svgTexts(const QByteArray &svg, QString *error)
{
    QXmlStreamReader xml(svg);
    QStringList texts;
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("text"))
            texts << xml.readElementText();
    }
    if (xml.hasError())
        *error = xml.errorString();
    return texts;
}

bool drawnOnWhite(const QImage &image)
{
    if (image.isNull() || image.pixelColor(0, 0) != QColor(Qt::white))
        return false;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            if (image.pixelColor(x, y) != QColor(Qt::white))
                return true;
        }
    }
    return false;
}

} // namespace

class TestPlanView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_view = std::make_unique<PlanView>();
        m_view->resize(1000, 700);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view.get()));
        const auto plan = catalog::parsePlan(Estimated);
        QVERIFY(plan);
        m_view->setPlan(*plan, QStringLiteral("SELECT"));
    }

    void cleanup() { m_view.reset(); }

    void modes()
    {
        QTabBar *tabs = m_view->modeTabs();
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->tabText(2), QStringLiteral("Diagram"));
        QCOMPARE(tabs->tabText(3), QStringLiteral("Flame Graph"));
        tabs->setCurrentIndex(2);
        QCOMPARE(m_view->viewMode(), PlanView::ViewMode::Diagram);
        QVERIFY(m_view->diagram()->isVisible());
        tabs->setCurrentIndex(3);
        QCOMPARE(m_view->viewMode(), PlanView::ViewMode::Flame);
        QVERIFY(m_view->flameGraph()->isVisible());
        QVERIFY(!m_view->diagram()->isVisible());
    }

    // A box per node; clicking one chooses it in the tree and the flame
    // graph too, and double-clicking goes to it in the tree.
    void diagram()
    {
        m_view->setViewMode(PlanView::ViewMode::Diagram);
        PlanDiagramView *diagram = m_view->diagram();
        QCOMPARE(diagram->nodeCount(), 4);
        const PlanNode *scan = &m_view->model()->plan().root.children[0];
        const QRect box = diagram->nodeRect(scan);
        QVERIFY(diagram->viewport()->rect().contains(box));
        QTest::mouseClick(diagram->viewport(), Qt::LeftButton, {}, box.center());
        QCOMPARE(diagram->currentNode(), scan);
        QCOMPARE(m_view->model()->nodeAt(m_view->tree()->currentIndex()), scan);
        QCOMPARE(m_view->flameGraph()->currentNode(), scan);

        // Chosen in the tree, chosen in the diagram.
        const PlanNode *root = &m_view->model()->plan().root;
        m_view->tree()->setCurrentIndex(m_view->model()->indexOf(root));
        QCOMPARE(diagram->currentNode(), root);

        QTest::mouseDClick(diagram->viewport(), Qt::LeftButton, {}, box.center());
        QCOMPARE(m_view->viewMode(), PlanView::ViewMode::Tree);
        QCOMPARE(m_view->model()->nodeAt(m_view->tree()->currentIndex()), scan);
    }

    // Each frame as wide as its share; a click widens one to the whole
    // width, and a click on the root narrows it back.
    void flame()
    {
        m_view->setViewMode(PlanView::ViewMode::Flame);
        FlameGraphView *flame = m_view->flameGraph();
        const PlanNode &root = m_view->model()->plan().root;
        const PlanNode *scan = &root.children[0];
        const PlanNode *hash = &root.children[1];
        const PlanNode *index = &hash->children[0];
        const int width = flame->width();
        QCOMPARE(flame->frameRect(&root).width(), width);
        QVERIFY(qAbs(flame->frameRect(scan).width() - width * 0.6) <= 1);
        // The root at the bottom, its children above it.
        QVERIFY(flame->frameRect(scan).bottom() < flame->frameRect(&root).top());
        QVERIFY(flame->frameRect(index).bottom() < flame->frameRect(hash).top());

        QTest::mouseClick(flame, Qt::LeftButton, {}, flame->frameRect(hash).center());
        QCOMPARE(flame->zoomedNode(), hash);
        QCOMPARE(flame->frameRect(hash).width(), width);
        QCOMPARE(flame->frameRect(index).width(), width);
        QVERIFY(flame->frameRect(scan).isEmpty()); // Beside it: out of sight.
        QCOMPARE(m_view->model()->nodeAt(m_view->tree()->currentIndex()), hash);

        // Chosen elsewhere where the zoom hides it: back to the whole plan.
        m_view->tree()->setCurrentIndex(m_view->model()->indexOf(scan));
        QCOMPARE(flame->zoomedNode(), &root);
        QVERIFY(!flame->frameRect(scan).isEmpty());

        QTest::mouseClick(flame, Qt::LeftButton, {}, flame->frameRect(hash).center());
        QTest::mouseClick(flame, Qt::LeftButton, {}, flame->frameRect(&root).center());
        QCOMPARE(flame->zoomedNode(), &root);
    }

    // A message, or another plan, leaves nothing of the last one behind.
    void replaced()
    {
        m_view->showMessage(QStringLiteral("error"), true);
        QCOMPARE(m_view->diagram()->nodeCount(), 0);
        QVERIFY(!m_view->flameGraph()->zoomedNode());

        catalog::Plan single;
        single.root.nodeType = QStringLiteral("Result");
        single.root.totalCost = 0.01;
        m_view->setPlan(single);
        QCOMPARE(m_view->diagram()->nodeCount(), 1);
        QCOMPARE(m_view->flameGraph()->zoomedNode(), &m_view->model()->plan().root);
        m_view->setViewMode(PlanView::ViewMode::Flame);
        QVERIFY(!m_view->flameGraph()->frameRect(&m_view->model()->plan().root).isEmpty());
    }

    // As the ER diagram saves: pictures as drawn, on white, and the text
    // other tools draw.
    void savesTheDiagram()
    {
        PlanDiagramView *diagram = m_view->diagram();
        QString error;
        const QString svgPath = m_dir.filePath(QStringLiteral("plan.svg"));
        QVERIFY2(diagram->saveDiagram(svgPath, &error), qPrintable(error));
        const QStringList texts = svgTexts(contentsOf(svgPath), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(texts.contains(QStringLiteral("Seq Scan")), qPrintable(texts.join(u'|')));
        QVERIFY(texts.contains(QStringLiteral("public.orders o")));
        QVERIFY(!contentsOf(svgPath).contains("<image"));

        const QString png = m_dir.filePath(QStringLiteral("plan.png"));
        QVERIFY2(diagram->saveDiagram(png, &error), qPrintable(error));
        QVERIFY(drawnOnWhite(QImage(png)));
        const QString pdf = m_dir.filePath(QStringLiteral("plan.pdf"));
        QVERIFY2(diagram->saveDiagram(pdf, &error), qPrintable(error));
        QVERIFY(contentsOf(pdf).startsWith("%PDF-"));

        const catalog::Plan &plan = m_view->model()->plan();
        const QString dot = m_dir.filePath(QStringLiteral("plan.gv"));
        QVERIFY2(diagram->saveDiagram(dot, &error), qPrintable(error));
        QCOMPARE(QString::fromUtf8(contentsOf(dot)), catalog::planToDot(plan));
        const QString mmd = m_dir.filePath(QStringLiteral("plan.mmd"));
        QVERIFY2(diagram->saveDiagram(mmd, &error), qPrintable(error));
        QCOMPARE(QString::fromUtf8(contentsOf(mmd)), catalog::planToMermaid(plan));

        QVERIFY(!diagram->saveDiagram(m_dir.filePath(QStringLiteral("plan.bmp")), &error));
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("plan.bmp"))));
    }

    void savesTheFlameGraph()
    {
        FlameGraphView *flame = m_view->flameGraph();
        QString error;
        const QString svgPath = m_dir.filePath(QStringLiteral("flame.svg"));
        QVERIFY2(flame->saveFlameGraph(svgPath, &error), qPrintable(error));
        const QStringList texts = svgTexts(contentsOf(svgPath), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(texts.join(u'|').contains(QStringLiteral("Hash Left Join · cost 100.00")),
                 qPrintable(texts.join(u'|')));

        const QString png = m_dir.filePath(QStringLiteral("flame.png"));
        QVERIFY2(flame->saveFlameGraph(png, &error), qPrintable(error));
        const QImage image(png);
        QVERIFY(drawnOnWhite(image));
        // Only as tall as the three rows of frames, at twice the size.
        QVERIFY2(image.height() < 3 * 2 * (flame->fontMetrics().height() + 6) + 2,
                 qPrintable(QString::number(image.height())));
        QVERIFY2(flame->saveFlameGraph(m_dir.filePath(QStringLiteral("flame.pdf")), &error),
                 qPrintable(error));

        const QString folded = m_dir.filePath(QStringLiteral("plan.folded"));
        QVERIFY2(flame->saveFlameGraph(folded, &error), qPrintable(error));
        QCOMPARE(QString::fromUtf8(contentsOf(folded)),
                 catalog::planToFoldedStacks(m_view->model()->plan()));
        QVERIFY(!flame->saveFlameGraph(m_dir.filePath(QStringLiteral("plan.mmd")), &error));
    }

    // Saving draws in its own colours, then puts the view back as it was.
    void exportLeavesTheViewAsItWas()
    {
        PlanDiagramView *diagram = m_view->diagram();
        const PlanNode *scan = &m_view->model()->plan().root.children[0];
        diagram->setCurrentNode(scan);
        const QPalette before = diagram->scene()->palette();
        QVERIFY(diagram->saveDiagram(m_dir.filePath(QStringLiteral("again.png"))));
        QCOMPARE(diagram->currentNode(), scan);
        QCOMPARE(diagram->scene()->palette(), before);
    }

    // The menu copies the text formats, and has nothing to offer without a
    // plan.
    void exportMenu()
    {
        auto *menu = m_view->exportButton()->menu();
        QVERIFY(menu);
        auto *copyMermaid = menu->findChild<QAction *>(QStringLiteral("copyMermaid"));
        QVERIFY(copyMermaid);
        Q_EMIT menu->aboutToShow();
        QVERIFY(copyMermaid->isEnabled());
        copyMermaid->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 catalog::planToMermaid(m_view->model()->plan()));
        menu->findChild<QAction *>(QStringLiteral("copyFolded"))->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 catalog::planToFoldedStacks(m_view->model()->plan()));

        m_view->showMessage(QStringLiteral("nothing"));
        Q_EMIT menu->aboutToShow();
        QVERIFY(!copyMermaid->isEnabled());
        QString error;
        QVERIFY(!m_view->diagram()->saveDiagram(m_dir.filePath(QStringLiteral("x.svg")), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!m_view->flameGraph()->saveFlameGraph(m_dir.filePath(QStringLiteral("x.svg"))));
    }

private:
    QTemporaryDir m_dir;
    std::unique_ptr<PlanView> m_view;
};

QTEST_MAIN(TestPlanView)
#include "tst_planview.moc"
