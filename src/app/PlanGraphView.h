// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/PlanGraph.h"

#include <QGraphicsView>
#include <QWidget>

class QGraphicsScene;

namespace slonisko {

class PlanBoxItem;

// The plan as a diagram: a box per node, the root at the top, the rows each
// node hands its parent drawn as a line as thick as there are many of them.
// The nodes point into the Plan given, which has to outlive the view's use
// of it.
class PlanDiagramView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit PlanDiagramView(QWidget *parent = nullptr);

    void setPlan(const catalog::Plan &plan);
    void clear();
    void setCurrentNode(const catalog::PlanNode *node);
    const catalog::PlanNode *currentNode() const;
    // Where the node's box is in the viewport; empty if it has none.
    QRect nodeRect(const catalog::PlanNode *node) const;
    int nodeCount() const { return int(m_boxes.size()); }

    // Saves the diagram as the file name's suffix says: .svg, .png and .pdf
    // as it is drawn, on white; .dot (or .gv) and .mmd as Graphviz and
    // Mermaid source, which those tools lay out their own way.
    bool saveDiagram(const QString &path, QString *error = nullptr);
    // The suffixes saveDiagram() knows, for a file dialog.
    static QStringList fileFilters();

    void zoomIn() { zoomBy(1.25); }
    void zoomOut() { zoomBy(1 / 1.25); }

Q_SIGNALS:
    void nodeClicked(const catalog::PlanNode *node);
    void nodeActivated(const catalog::PlanNode *node); // Double-clicked.

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    PlanBoxItem *boxAt(const QPoint &pos) const;
    void zoomBy(qreal factor);
    // Shrunk to fit if it has to be, as far as it stays readable, the root
    // in sight.
    void fitPlan();

    const catalog::Plan *m_plan = nullptr;
    QGraphicsScene *m_scene = nullptr;
    std::vector<PlanBoxItem *> m_boxes;
    bool m_userAdjusted = false; // Zoomed by hand: left as it is.
};

// The plan as a flame graph: a frame per node, as wide as the time spent in
// it and the nodes under it, those above it. Clicking a frame widens it to
// the whole width; clicking one below it, down to the root, narrows it back.
class FlameGraphView : public QWidget
{
    Q_OBJECT

public:
    explicit FlameGraphView(QWidget *parent = nullptr);

    void setPlan(const catalog::Plan &plan);
    void clear();
    void setCurrentNode(const catalog::PlanNode *node);
    const catalog::PlanNode *currentNode() const;
    // The frame widened to the whole width; the root's at first.
    const catalog::PlanNode *zoomedNode() const;
    void zoomTo(const catalog::PlanNode *node);
    // Where the node's frame is drawn now; empty if it is out of sight.
    QRect frameRect(const catalog::PlanNode *node) const;

    // Saves the frames shown, zoomed as they are, as .svg, .png or .pdf; or
    // the whole plan as .folded stacks for flamegraph.pl, speedscope and
    // the like.
    bool saveFlameGraph(const QString &path, QString *error = nullptr);
    static QStringList fileFilters();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void nodeClicked(const catalog::PlanNode *node);
    void nodeActivated(const catalog::PlanNode *node);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    int rowHeight() const;
    int indexOf(const catalog::PlanNode *node) const;
    int frameAt(const QPoint &pos) const;
    QRectF frameGeometry(int index) const;
    // Where a frame goes in a picture of that size, the root at its bottom.
    QRectF frameGeometry(int index, const QSizeF &area) const;
    void paintFrames(QPainter &painter, const QSizeF &area, const QPalette &palette,
                     bool showCurrent) const;

    const catalog::Plan *m_plan = nullptr;
    std::vector<catalog::FlameFrame> m_frames;
    int m_zoom = 0; // Index of the frame shown whole width.
    int m_current = -1;
};

} // namespace slonisko
