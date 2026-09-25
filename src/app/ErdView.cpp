// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ErdView.h"

#include <QApplication>
#include <QEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QWheelEvent>

#include <algorithm>

namespace slonisko {

using catalog::ErdColumn;
using catalog::ErdEdge;
using catalog::ErdNode;

namespace {

constexpr qreal MinScale = 0.15;
constexpr qreal MaxScale = 3.0;
// A diagram that opens smaller than this is not worth reading; it is better
// to show the focus table at a sensible size and let the rest be scrolled to.
constexpr qreal MinOpeningScale = 0.7;
// Below this the columns are not readable anyway, so only the name is drawn.
constexpr qreal NamesOnlyDetail = 0.55;

QColor mix(const QColor &a, const QColor &b, float part)
{
    return QColor::fromRgbF(a.redF() * (1 - part) + b.redF() * part,
                            a.greenF() * (1 - part) + b.greenF() * part,
                            a.blueF() * (1 - part) + b.blueF() * part);
}

} // namespace

// One table: header with its name, then a line per column. Nothing about it
// is a child item, so a diagram of 200 tables is 200 items.
class ErdTableItem : public QGraphicsItem
{
public:
    ErdTableItem(const ErdNode &node, const catalog::ErdMetrics &metrics, bool focus)
        : m_node(node), m_metrics(metrics), m_focus(focus)
    {
        m_size = metrics.sizeOf(node);
        setFlags(ItemIsSelectable | ItemIsMovable | ItemSendsGeometryChanges);
        setCacheMode(DeviceCoordinateCache);
        setAcceptHoverEvents(true);
        setZValue(1);
        QStringList tip;
        tip << node.qualifiedName();
        for (const ErdColumn &column : node.columns) {
            tip << QStringLiteral("%1  %2%3")
                       .arg(column.name, column.type,
                            column.primaryKey       ? QStringLiteral("  (primary key)")
                                : column.foreignKey ? QStringLiteral("  (foreign key)")
                                                    : QString());
        }
        if (node.otherColumns > 0)
            tip << ErdView::tr("and %1 columns that are not keys").arg(node.otherColumns);
        setToolTip(tip.join(QLatin1Char('\n')));
    }

    unsigned int oid() const { return m_node.oid; }
    bool isFocus() const { return m_focus; }
    QRectF boundingRect() const override
    {
        return QRectF(QPointF(), m_size).adjusted(-1, -1, 1, 1);
    }

    QRectF box() const { return QRectF(scenePos(), m_size); }

    // Where an edge meets this table. Sideways it leaves at the column's own
    // row, which is what makes a diagram readable; above or below there is no
    // row to aim at, so it leaves in the middle.
    QPointF sideAnchor(const QString &column, bool right) const
    {
        const QRectF area = box();
        const int row = rowOf(column);
        const qreal y = row < 0
            ? area.center().y()
            : area.top() + m_metrics.headerHeight + m_metrics.rowHeight * (row + 0.5);
        return {right ? area.right() : area.left(), std::min(y, area.bottom() - 2)};
    }

    QPointF verticalAnchor(bool bottom) const
    {
        const QRectF area = box();
        return {area.center().x(), bottom ? area.bottom() : area.top()};
    }

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *) override
    {
        const QPalette palette = QApplication::palette();
        const QRectF box(QPointF(), m_size);
        const QColor base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);
        const QColor line = mix(base, text, 0.35f);
        const bool selected = option->state & QStyle::State_Selected;

        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(selected ? palette.color(QPalette::Highlight) : line,
                             selected || m_focus ? 2 : 1));
        painter->setBrush(base);
        painter->drawRoundedRect(box, 4, 4);

        // The header carries the name, and says which table the diagram is about.
        QRectF header(box.topLeft(), QSizeF(box.width(), m_metrics.headerHeight));
        painter->setBrush(m_focus ? mix(base, palette.color(QPalette::Highlight), 0.35f)
                                  : mix(base, palette.color(QPalette::Window), 0.7f));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(header, 4, 4);
        painter->fillRect(QRectF(header.left(), header.bottom() - 4, header.width(), 4),
                          painter->brush());
        painter->setPen(line);
        painter->drawLine(header.bottomLeft(), header.bottomRight());

        const QFont plain = painter->font();
        QFont bold = plain;
        bold.setBold(true);
        painter->setFont(bold);
        painter->setPen(text);
        const qreal pad = m_metrics.padding;
        painter->drawText(header.adjusted(pad, 0, -pad, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          painter->fontMetrics().elidedText(m_node.name, Qt::ElideRight,
                                                            int(box.width() - 2 * pad)));
        painter->setFont(plain);

        if (option->levelOfDetailFromTransform(painter->worldTransform()) < NamesOnlyDetail)
            return; // Too small to read: the name says enough.

        const int rows = m_metrics.maxRows > 0
            ? std::min(int(m_node.columns.size()), m_metrics.maxRows)
            : int(m_node.columns.size());
        qreal y = box.top() + m_metrics.headerHeight;
        for (int row = 0; row < rows; ++row) {
            const ErdColumn &column = m_node.columns[std::size_t(row)];
            const QRectF area(box.left() + pad, y, box.width() - 2 * pad, m_metrics.rowHeight);
            QFont font = plain;
            font.setBold(column.primaryKey);
            font.setItalic(column.foreignKey);
            painter->setFont(font);
            painter->setPen(text);
            painter->drawText(area, Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(column.name, Qt::ElideRight,
                                                                int(area.width() * 0.6)));
            painter->setFont(plain);
            painter->setPen(mix(base, text, 0.6f));
            painter->drawText(area, Qt::AlignVCenter | Qt::AlignRight,
                              painter->fontMetrics().elidedText(column.type, Qt::ElideRight,
                                                                int(area.width() * 0.4)));
            y += m_metrics.rowHeight;
        }
        // Only key columns are drawn, so the box says what it leaves out.
        const int hidden = int(m_node.columns.size()) - rows + m_node.otherColumns;
        if (hidden > 0) {
            QFont italic = plain;
            italic.setItalic(true);
            painter->setFont(italic);
            painter->setPen(mix(base, text, 0.6f));
            painter->drawText(
                QRectF(box.left() + pad, y, box.width() - 2 * pad, m_metrics.rowHeight),
                Qt::AlignVCenter | Qt::AlignLeft,
                hidden == 1 ? ErdView::tr("+ 1 column") : ErdView::tr("+ %1 columns").arg(hidden));
            painter->setFont(plain);
        }
    }

    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
    {
        if (change == ItemPositionHasChanged && scene())
            scene()->update(); // The edges follow the box.
        return QGraphicsItem::itemChange(change, value);
    }

private:
    int rowOf(const QString &column) const
    {
        const int rows = m_metrics.maxRows > 0
            ? std::min(int(m_node.columns.size()), m_metrics.maxRows)
            : int(m_node.columns.size());
        for (int row = 0; row < rows; ++row) {
            if (m_node.columns[std::size_t(row)].name == column)
                return row;
        }
        return -1;
    }

    ErdNode m_node;
    catalog::ErdMetrics m_metrics;
    bool m_focus = false;
    QSizeF m_size;
};

namespace {

// A foreign key, drawn from the referencing column to the referenced one.
class ErdEdgeItem : public QGraphicsItem
{
public:
    ErdEdgeItem(const ErdEdge &edge, ErdTableItem *from, ErdTableItem *to)
        : m_edge(edge), m_from(from), m_to(to)
    {
        setZValue(0);
        setToolTip(QStringLiteral("%1\n%2 (%3) → %4 (%5)")
                       .arg(edge.name, from->toolTip().section(QLatin1Char('\n'), 0, 0),
                            edge.fromColumns.join(QStringLiteral(", ")),
                            to->toolTip().section(QLatin1Char('\n'), 0, 0),
                            edge.toColumns.join(QStringLiteral(", "))));
    }

    QRectF boundingRect() const override
    {
        return path().controlPointRect().adjusted(-8, -8, 8, 8);
    }

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const QPalette palette = QApplication::palette();
        const QColor color
            = mix(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.55f);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(color, 1.2));
        painter->setBrush(Qt::NoBrush);
        const QPainterPath line = path();
        painter->drawPath(line);

        // A crow's foot at the referencing end, a bar at the referenced one.
        const QPointF many = line.pointAtPercent(0.0);
        const QPointF one = line.pointAtPercent(1.0);
        const qreal angle = std::atan2(line.pointAtPercent(0.05).y() - many.y(),
                                       line.pointAtPercent(0.05).x() - many.x());
        painter->setBrush(color);
        for (const qreal spread : {-0.5, 0.0, 0.5}) {
            painter->drawLine(
                many, many + QPointF(std::cos(angle + spread) * 9, std::sin(angle + spread) * 9));
        }
        const qreal back = std::atan2(one.y() - line.pointAtPercent(0.95).y(),
                                      one.x() - line.pointAtPercent(0.95).x());
        painter->drawLine(one
                              - QPointF(std::cos(back) * 6 - std::sin(back) * 5,
                                        std::sin(back) * 6 + std::cos(back) * 5),
                          one
                              - QPointF(std::cos(back) * 6 + std::sin(back) * 5,
                                        std::sin(back) * 6 - std::cos(back) * 5));
    }

private:
    QPainterPath path() const
    {
        QPainterPath path;
        const QRectF from = m_from->box();
        const QRectF to = m_to->box();
        if (m_edge.isSelfReference()) {
            // A loop out of the right side and back in again.
            const QPointF out(from.right(), from.top() + from.height() * 0.35);
            const QPointF in(from.right(), from.top() + from.height() * 0.65);
            path.moveTo(out);
            path.cubicTo(out + QPointF(40, -10), in + QPointF(40, 10), in);
            return path;
        }

        // One above the other: leave through the near horizontal edge, so
        // the line runs straight down the gap instead of around the boxes.
        const bool sideBySide = from.right() < to.left() || to.right() < from.left();
        if (!sideBySide) {
            const bool downwards = to.center().y() > from.center().y();
            const QPointF start = m_from->verticalAnchor(downwards);
            const QPointF end = m_to->verticalAnchor(!downwards);
            const qreal reach = std::max<qreal>(24, std::abs(end.y() - start.y()) / 3);
            path.moveTo(start);
            path.cubicTo(start + QPointF(0, downwards ? reach : -reach),
                         end + QPointF(0, downwards ? -reach : reach), end);
            return path;
        }

        // Side by side: out of the near side at the key's own row.
        const bool rightwards = to.center().x() > from.center().x();
        const QPointF start = m_from->sideAnchor(m_edge.fromColumns.value(0), rightwards);
        const QPointF end = m_to->sideAnchor(m_edge.toColumns.value(0), !rightwards);
        const qreal reach = std::max<qreal>(30, std::abs(end.x() - start.x()) / 3);
        path.moveTo(start);
        path.cubicTo(start + QPointF(rightwards ? reach : -reach, 0),
                     end + QPointF(rightwards ? -reach : reach, 0), end);
        return path;
    }

    ErdEdge m_edge;
    ErdTableItem *m_from = nullptr;
    ErdTableItem *m_to = nullptr;
};

} // namespace

ErdView::ErdView(QWidget *parent) : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setFrameShape(QFrame::NoFrame);
    m_scene->setItemIndexMethod(QGraphicsScene::NoIndex); // Tables are dragged about.
}

void ErdView::setGraph(const catalog::ErdGraph &graph)
{
    m_graph = graph;
    rebuild();
}

void ErdView::clear()
{
    m_graph = {};
    rebuild();
}

catalog::ErdMetrics ErdView::metrics() const
{
    catalog::ErdMetrics metrics;
    const QFontMetricsF fm(font());
    metrics.charWidth = fm.horizontalAdvance(QStringLiteral("n"));
    metrics.rowHeight = fm.height() + 4;
    metrics.headerHeight = fm.height() + 10;
    metrics.minWidth = 24 * metrics.charWidth;
    metrics.maxWidth = 46 * metrics.charWidth;
    metrics.maxRows = 18; // Long tables would make a wall of text.
    return metrics;
}

void ErdView::rebuild()
{
    m_scene->clear(); // Deletes the items; the hash goes with them.
    m_tables.clear();
    if (m_graph.isEmpty()) {
        m_scene->setSceneRect(QRectF());
        return;
    }

    const catalog::ErdMetrics m = metrics();
    const catalog::ErdLayout layout = catalog::starLayout(m_graph, m);
    for (const catalog::ErdPlacement &placement : layout.nodes) {
        const catalog::ErdNode *node = m_graph.node(placement.oid);
        if (!node)
            continue;
        auto *item = new ErdTableItem(*node, m, node->oid == m_graph.focus);
        item->setPos(placement.box.topLeft());
        m_scene->addItem(item);
        m_tables.insert(node->oid, item);
    }
    for (const catalog::ErdEdge &edge : m_graph.edges) {
        ErdTableItem *from = m_tables.value(edge.from);
        ErdTableItem *to = m_tables.value(edge.to);
        if (from && to)
            m_scene->addItem(new ErdEdgeItem(edge, from, to));
    }

    // From the items, not from the layout: edges curve out beyond the boxes.
    m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
    m_userAdjusted = false;
    focusDiagram();
}

QRectF ErdView::focusBox() const
{
    const ErdTableItem *focus = m_tables.value(m_graph.focus);
    return focus ? focus->box() : QRectF();
}

void ErdView::fitDiagram()
{
    if (m_scene->sceneRect().isNull())
        return;
    fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
    // Fitting may blow a small diagram up; it should never look zoomed in.
    if (transform().m11() > 1.0)
        setTransform(QTransform());
    centerOn(m_scene->sceneRect().center());
    m_userAdjusted = true;
}

void ErdView::focusDiagram()
{
    const QRectF scene = m_scene->sceneRect();
    if (scene.isNull() || !viewport() || viewport()->width() <= 1)
        return; // Not on screen yet: showEvent comes back to this.

    const qreal fits
        = std::min(viewport()->width() / scene.width(), viewport()->height() / scene.height());
    // Never bigger than life size, never so small that the columns go.
    const qreal wanted = std::clamp(fits, MinOpeningScale, 1.0);
    setTransform(QTransform::fromScale(wanted, wanted));

    const QRectF focus = focusBox();
    centerOn(focus.isNull() ? scene.center() : focus.center());
}

void ErdView::resetZoom()
{
    setTransform(QTransform());
    const QRectF focus = focusBox();
    centerOn(focus.isNull() ? m_scene->sceneRect().center() : focus.center());
    m_userAdjusted = true;
}

void ErdView::showEvent(QShowEvent *event)
{
    QGraphicsView::showEvent(event);
    // The viewport has its real size only now: what was worked out while the
    // tab was hidden would be the wrong scale.
    if (!m_userAdjusted)
        focusDiagram();
}

void ErdView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (!m_userAdjusted)
        focusDiagram();
}

void ErdView::zoomBy(qreal factor)
{
    const qreal wanted = std::clamp(transform().m11() * factor, MinScale, MaxScale);
    setTransform(QTransform::fromScale(wanted, wanted));
    m_userAdjusted = true;
}

void ErdView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1 / 1.15);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ErdView::mousePressEvent(QMouseEvent *event)
{
    m_userAdjusted = true; // Panning and moving tables are the user's doing.
    QGraphicsView::mousePressEvent(event);
}

void ErdView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (auto *table = dynamic_cast<ErdTableItem *>(itemAt(event->position().toPoint()))) {
        if (!table->isFocus()) {
            Q_EMIT tableActivated(table->oid());
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ErdView::changeEvent(QEvent *event)
{
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        m_scene->update();
}

} // namespace slonisko
