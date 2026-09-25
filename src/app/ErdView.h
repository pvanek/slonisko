// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Erd.h"

#include <QGraphicsView>

class QGraphicsScene;

namespace slonisko {

class ErdTableItem;

// A diagram of tables and the foreign keys between them. One item per table,
// its columns painted inside it: a box per column would be thousands of
// items in a schema-wide diagram.
class ErdView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit ErdView(QWidget *parent = nullptr);

    void setGraph(const catalog::ErdGraph &graph);
    const catalog::ErdGraph &graph() const { return m_graph; }
    // Where the table the diagram is about sits in the scene, as laid out
    // and wherever it has been dragged to since; empty if there is none.
    QRectF focusBox() const;
    void clear();

    // Everything in sight, however small that makes it.
    void fitDiagram();
    // The table the diagram is about, in the middle and readable. What the
    // diagram opens with: fitting a wide one shrinks it past reading size.
    void focusDiagram();
    void zoomIn() { zoomBy(1.25); }
    void zoomOut() { zoomBy(1 / 1.25); }
    void resetZoom();

Q_SIGNALS:
    // A table other than the focus was double-clicked.
    void tableActivated(unsigned int oid);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    catalog::ErdMetrics metrics() const;
    void rebuild();
    void zoomBy(qreal factor);

    QGraphicsScene *m_scene = nullptr;
    catalog::ErdGraph m_graph;
    QHash<unsigned int, ErdTableItem *> m_tables;
    // Until the user zooms or pans, the view keeps putting the focus table
    // in the middle; afterwards it leaves the view where they left it.
    bool m_userAdjusted = false;
};

} // namespace slonisko
