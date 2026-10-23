// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlanGraphView.h"

#include "DiagramFile.h"
#include "catalog/PlanExport.h"

#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>

namespace slonisko {

using catalog::flameDepth;
using catalog::PlanNode;

namespace {

constexpr qreal MinScale = 0.2;
constexpr qreal MaxScale = 3.0;
constexpr qreal MaxEdgeWidth = 12;
// A plan opens shrunk to fit, but not past reading; the rest is scrolled to.
constexpr qreal MinOpeningScale = 0.6;

QColor mix(const QColor &a, const QColor &b, float part)
{
    return QColor::fromRgbF(a.redF() * (1 - part) + b.redF() * part,
                            a.greenF() * (1 - part) + b.greenF() * part,
                            a.blueF() * (1 - part) + b.blueF() * part);
}

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightness() < 128;
}

// Yellow for a node that takes nothing of its own, through orange to red
// for one that takes all of it; darker on a dark background, where a light
// fill would glare.
QColor heat(double share, bool dark)
{
    const double hue = 55.0 / 360 * (1 - std::clamp(share, 0.0, 1.0));
    return QColor::fromHslF(float(hue), 0.85f, dark ? 0.36f : 0.70f);
}

QString shareText(double share)
{
    return QStringLiteral("%1 %").arg(share * 100, 0, 'f', 1);
}

} // namespace

// PlanBoxItem: one node. Painted whole, text and all; there is no child item.

class PlanBoxItem : public QGraphicsItem
{
public:
    PlanBoxItem(const PlanNode *node, const QRectF &rect, const QFont &font)
        : m_node(node), m_rect(rect), m_font(font)
    {
        setFlag(ItemIsSelectable);
        setPos(rect.topLeft());
        setToolTip(catalog::planNodeTooltip(*node));
        setCacheMode(DeviceCoordinateCache);
    }

    const PlanNode *node() const { return m_node; }
    QRectF boundingRect() const override { return QRectF(QPointF(), m_rect.size()); }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const QPalette palette = scene()->palette();
        const bool dark = isDark(palette);
        const QColor base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);
        const QRectF box = boundingRect().adjusted(1, 1, -1, -1);
        const double share = m_node->exclusiveShare;

        // Tinted by its own share, so the expensive ones stand out at any zoom.
        const QColor fill = share >= 0.05 ? mix(base, heat(share, dark), float(0.2 + 0.5 * share))
                                          : mix(base, palette.color(QPalette::Window), 0.6f);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(isSelected() ? QPen(palette.color(QPalette::Highlight), 2.5)
                                     : QPen(mix(base, text, 0.35f), 1));
        painter->setBrush(fill);
        painter->drawRoundedRect(box, 4, 4);

        const QFontMetricsF fm(m_font);
        const qreal pad = 6;
        const qreal width = box.width() - 2 * pad;
        qreal y = box.top() + pad;
        auto line = [&](const QString &s, const QColor &color, bool bold = false) {
            QFont f = m_font;
            f.setBold(bold);
            painter->setFont(f);
            painter->setPen(color);
            painter->drawText(QRectF(box.left() + pad, y, width, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetricsF(f).elidedText(s, Qt::ElideRight, width));
            y += fm.height();
        };
        const QColor dim = mix(fill, text, 0.7f);
        const QStringList lines = catalog::planBoxLines(*m_node);
        line(lines.value(0), text, true);
        line(lines.value(1), dim);
        line(lines.value(2), text);
        line(lines.value(3),
             catalog::rowsMisestimated(*m_node)
                 ? (dark ? QColor(255, 170, 80) : QColor(200, 100, 0))
                 : dim);

        // The own share as a bar along the bottom.
        if (share > 0) {
            const QRectF bar(box.left() + 1, box.bottom() - 4, (box.width() - 2) * share, 3);
            painter->fillRect(bar, heat(share, dark).darker(dark ? 80 : 120));
        }
    }

private:
    const PlanNode *m_node;
    QRectF m_rect;
    QFont m_font;
};

namespace {

// The rows a node hands its parent, from the top of its box to the bottom
// of the parent's.
class PlanEdgeItem : public QGraphicsPathItem
{
public:
    PlanEdgeItem(const QRectF &child, const QRectF &parent, double width, double rows)
    {
        const QPointF from(child.center().x(), child.top());
        const QPointF to(parent.center().x(), parent.bottom());
        const qreal middle = (from.y() + to.y()) / 2;
        QPainterPath path(from);
        path.cubicTo(QPointF(from.x(), middle), QPointF(to.x(), middle), to);
        setPath(path);
        m_width = width;
        setZValue(-1);
        setToolTip(QObject::tr("%1 rows").arg(qint64(rows)));
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const QPalette palette = scene()->palette();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(
            QPen(mix(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.35f), m_width,
                 Qt::SolidLine, Qt::FlatCap));
        painter->drawPath(path());
    }

    QRectF boundingRect() const override
    {
        return path().boundingRect().adjusted(-m_width, -m_width, m_width, m_width);
    }

private:
    double m_width = 1;
};

} // namespace

// PlanDiagramView.

PlanDiagramView::PlanDiagramView(QWidget *parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setAlignment(Qt::AlignHCenter | Qt::AlignTop);
}

void PlanDiagramView::clear()
{
    m_plan = nullptr;
    m_boxes.clear();
    m_scene->clear();
    m_scene->setSceneRect(QRectF());
}

void PlanDiagramView::setPlan(const catalog::Plan &plan)
{
    clear();
    m_plan = &plan;
    const QFontMetricsF fm(font());
    const QSizeF box(std::round(fm.averageCharWidth() * 34), std::round(fm.height() * 4 + 16));
    const QSizeF gap(std::round(fm.averageCharWidth() * 3), std::round(fm.height() * 2.5));
    const catalog::PlanDiagramLayout layout = catalog::layoutPlanDiagram(plan, box, gap);
    for (const catalog::PlanDiagramBox &b : layout.boxes) {
        auto *item = new PlanBoxItem(b.node, b.rect, font());
        m_scene->addItem(item);
        m_boxes.push_back(item);
        if (b.parent >= 0) {
            const double rows = catalog::outputRows(*b.node);
            m_scene->addItem(
                new PlanEdgeItem(b.rect, layout.boxes[std::size_t(b.parent)].rect,
                                 catalog::edgeWidth(rows, layout.maxRows, MaxEdgeWidth), rows));
        }
    }
    const qreal margin = fm.height();
    m_scene->setSceneRect(
        QRectF(QPointF(), layout.size).adjusted(-margin, -margin, margin, margin));
    m_userAdjusted = false;
    fitPlan();
}

void PlanDiagramView::fitPlan()
{
    if (m_boxes.empty())
        return;
    const QRectF scene = m_scene->sceneRect();
    const QSize view = viewport()->size();
    const qreal fit = std::min(view.width() / scene.width(), view.height() / scene.height());
    const qreal scale = std::clamp(fit, MinOpeningScale, 1.0);
    setTransform(QTransform::fromScale(scale, scale));
    ensureVisible(m_boxes.front());
}

void PlanDiagramView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    // Until the user zooms, it keeps fitting; a hidden tab has no size yet.
    if (!m_userAdjusted)
        fitPlan();
}

void PlanDiagramView::setCurrentNode(const PlanNode *node)
{
    const QSignalBlocker blocker(m_scene);
    m_scene->clearSelection();
    for (PlanBoxItem *box : m_boxes) {
        if (box->node() == node) {
            box->setSelected(true);
            ensureVisible(box, 20, 20);
        }
    }
}

const PlanNode *PlanDiagramView::currentNode() const
{
    for (PlanBoxItem *box : m_boxes) {
        if (box->isSelected())
            return box->node();
    }
    return nullptr;
}

QRect PlanDiagramView::nodeRect(const PlanNode *node) const
{
    for (PlanBoxItem *box : m_boxes) {
        if (box->node() == node)
            return mapFromScene(box->sceneBoundingRect()).boundingRect();
    }
    return {};
}

QStringList PlanDiagramView::fileFilters()
{
    return {tr("SVG image (*.svg)"), tr("PNG image (*.png)"), tr("PDF document (*.pdf)"),
            tr("Graphviz (*.dot *.gv)"), tr("Mermaid (*.mmd)")};
}

bool PlanDiagramView::saveDiagram(const QString &path, QString *error)
{
    if (!m_plan || m_boxes.empty()) {
        if (error)
            *error = tr("There is no plan to save.");
        return false;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (isPictureFile(path)) {
        const QRectF from = m_scene->itemsBoundingRect().adjusted(-20, -20, 20, 20);
        return savePicture(
            path, from.size(), logicalDpiY(),
            [this, from](QPainter *painter, const QRectF &target) {
                const QList<QGraphicsItem *> selected = m_scene->selectedItems();
                m_scene->clearSelection();
                m_scene->setPalette(exportPalette());
                // A cached box would go out as a picture of itself, not as
                // lines and text a vector file keeps sharp.
                for (PlanBoxItem *box : m_boxes)
                    box->setCacheMode(QGraphicsItem::NoCache);
                painter->fillRect(target, Qt::white);
                m_scene->render(painter, target, from);
                for (PlanBoxItem *box : m_boxes)
                    box->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
                m_scene->setPalette(palette());
                for (QGraphicsItem *item : selected)
                    item->setSelected(true);
            },
            error);
    }
    if (suffix == QLatin1String("dot") || suffix == QLatin1String("gv"))
        return saveText(path, catalog::planToDot(*m_plan).toUtf8(), error);
    if (suffix == QLatin1String("mmd"))
        return saveText(path, catalog::planToMermaid(*m_plan).toUtf8(), error);
    if (error)
        *error = tr("Unknown file type: choose .svg, .png, .pdf, .dot or .mmd.");
    return false;
}

PlanBoxItem *PlanDiagramView::boxAt(const QPoint &pos) const
{
    for (QGraphicsItem *item : items(pos)) {
        if (auto *box = dynamic_cast<PlanBoxItem *>(item))
            return box;
    }
    return nullptr;
}

void PlanDiagramView::zoomBy(qreal factor)
{
    const qreal wanted = std::clamp(transform().m11() * factor, MinScale, MaxScale);
    setTransform(QTransform::fromScale(wanted, wanted));
    m_userAdjusted = true;
}

void PlanDiagramView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1 / 1.15);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void PlanDiagramView::mousePressEvent(QMouseEvent *event)
{
    QGraphicsView::mousePressEvent(event);
    if (event->button() != Qt::LeftButton)
        return;
    if (PlanBoxItem *box = boxAt(event->position().toPoint())) {
        setCurrentNode(box->node());
        Q_EMIT nodeClicked(box->node());
    }
}

void PlanDiagramView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (PlanBoxItem *box = boxAt(event->position().toPoint())) {
        Q_EMIT nodeActivated(box->node());
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void PlanDiagramView::changeEvent(QEvent *event)
{
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        m_scene->setPalette(palette());
        // The boxes are cached as drawn in the old colours.
        for (PlanBoxItem *box : m_boxes)
            box->update();
    }
}

// FlameGraphView.

FlameGraphView::FlameGraphView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
}

void FlameGraphView::setPlan(const catalog::Plan &plan)
{
    m_plan = &plan;
    m_frames = catalog::flameGraph(plan);
    m_zoom = 0;
    m_current = -1;
    updateGeometry();
    update();
}

void FlameGraphView::clear()
{
    m_plan = nullptr;
    m_frames.clear();
    m_zoom = 0;
    m_current = -1;
    updateGeometry();
    update();
}

int FlameGraphView::rowHeight() const
{
    return fontMetrics().height() + 6;
}

QSize FlameGraphView::minimumSizeHint() const
{
    return QSize(100, catalog::flameDepth(m_frames) * rowHeight());
}

QSize FlameGraphView::sizeHint() const
{
    return QSize(600, catalog::flameDepth(m_frames) * rowHeight());
}

int FlameGraphView::indexOf(const PlanNode *node) const
{
    for (std::size_t i = 0; i < m_frames.size(); ++i) {
        if (m_frames[i].node == node)
            return int(i);
    }
    return -1;
}

void FlameGraphView::setCurrentNode(const PlanNode *node)
{
    m_current = indexOf(node);
    // One out of sight is brought into it, the way the other views scroll to it.
    if (m_current >= 0 && frameGeometry(m_current).width() < 1)
        m_zoom = 0;
    update();
}

const PlanNode *FlameGraphView::currentNode() const
{
    return m_current >= 0 ? m_frames[std::size_t(m_current)].node : nullptr;
}

const PlanNode *FlameGraphView::zoomedNode() const
{
    return m_frames.empty() ? nullptr : m_frames[std::size_t(m_zoom)].node;
}

void FlameGraphView::zoomTo(const PlanNode *node)
{
    m_zoom = std::max(0, indexOf(node));
    update();
}

QRectF FlameGraphView::frameGeometry(int index) const
{
    return frameGeometry(index, size());
}

QRectF FlameGraphView::frameGeometry(int index, const QSizeF &area) const
{
    const catalog::FlameFrame &zoom = m_frames[std::size_t(m_zoom)];
    const catalog::FlameFrame &f = m_frames[std::size_t(index)];
    if (zoom.width <= 0)
        return {};
    const qreal w = area.width();
    const qreal left = std::max(0.0, (f.start - zoom.start) / zoom.width * w);
    const qreal right = std::min(w, (f.start + f.width - zoom.start) / zoom.width * w);
    if (right - left < 0.5)
        return {};
    // The root at the bottom, as flame graphs have it.
    const int row = rowHeight();
    return QRectF(left, area.height() - (f.depth + 1) * row, right - left, row);
}

QRect FlameGraphView::frameRect(const PlanNode *node) const
{
    const int index = indexOf(node);
    return index < 0 ? QRect() : frameGeometry(index).toAlignedRect();
}

int FlameGraphView::frameAt(const QPoint &pos) const
{
    for (int i = int(m_frames.size()) - 1; i >= 0; --i) {
        if (frameGeometry(i).contains(pos))
            return i;
    }
    return -1;
}

void FlameGraphView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paintFrames(painter, size(), palette(), true);
}

void FlameGraphView::paintFrames(QPainter &painter, const QSizeF &area, const QPalette &palette,
                                 bool showCurrent) const
{
    const bool dark = isDark(palette);
    painter.fillRect(QRectF(QPointF(), area), palette.color(QPalette::Base));
    if (m_frames.empty())
        return;
    const double whole = m_frames.front().value;
    const int zoomDepth = m_frames[std::size_t(m_zoom)].depth;
    const QFontMetrics fm = fontMetrics();
    for (int i = 0; i < int(m_frames.size()); ++i) {
        const QRectF r = frameGeometry(i, area);
        if (r.isEmpty())
            continue;
        const catalog::FlameFrame &f = m_frames[std::size_t(i)];
        QColor fill = heat(f.node->exclusiveShare, dark);
        // Below the frame zoomed into: only where it sits, so fainter.
        if (f.depth < zoomDepth)
            fill = mix(fill, palette.color(QPalette::Base), 0.55f);
        const QRectF box = r.adjusted(0.5, 0.5, -0.5, -0.5);
        painter.fillRect(box, fill);
        painter.setPen(palette.color(QPalette::Base));
        painter.drawRect(box);
        if (box.width() > fm.averageCharWidth() * 3) {
            QString label = f.node->nodeType;
            if (!f.node->object.isEmpty())
                label += QLatin1Char(' ') + f.node->object;
            label += QStringLiteral(" · ") + catalog::planValueText(*f.node);
            if (whole > 0)
                label += QStringLiteral(" (%1)").arg(shareText(f.value / whole));
            painter.setPen(dark ? QColor(Qt::white) : QColor(Qt::black));
            const QRectF text = box.adjusted(4, 0, -4, 0);
            painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                             fm.elidedText(label, Qt::ElideRight, int(text.width())));
        }
        if (showCurrent && i == m_current) {
            painter.setPen(QPen(palette.color(QPalette::Highlight), 2));
            painter.drawRect(box.adjusted(1, 1, -1, -1));
        }
    }
}

QStringList FlameGraphView::fileFilters()
{
    return {tr("SVG image (*.svg)"), tr("PNG image (*.png)"), tr("PDF document (*.pdf)"),
            tr("Folded stacks (*.folded)")};
}

bool FlameGraphView::saveFlameGraph(const QString &path, QString *error)
{
    if (!m_plan || m_frames.empty()) {
        if (error)
            *error = tr("There is no plan to save.");
        return false;
    }
    if (isPictureFile(path)) {
        // Only as tall as the frames, and wide enough to read however
        // narrow the view.
        const QSizeF area(std::max(width(), 800), flameDepth(m_frames) * rowHeight());
        return savePicture(
            path, area, logicalDpiY(),
            [this, area](QPainter *painter, const QRectF &target) {
                painter->save();
                painter->translate(target.topLeft());
                painter->scale(target.width() / area.width(), target.height() / area.height());
                painter->setFont(font());
                paintFrames(*painter, area, exportPalette(), false);
                painter->restore();
            },
            error);
    }
    if (QFileInfo(path).suffix().toLower() == QLatin1String("folded"))
        return saveText(path, catalog::planToFoldedStacks(*m_plan).toUtf8(), error);
    if (error)
        *error = tr("Unknown file type: choose .svg, .png, .pdf or .folded.");
    return false;
}

bool FlameGraphView::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int i = frameAt(help->pos());
        if (i < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const catalog::FlameFrame &f = m_frames[std::size_t(i)];
        QString tip = catalog::planNodeTooltip(*f.node);
        if (m_frames.front().value > 0)
            tip += QLatin1Char('\n')
                + tr("With the nodes under it: %1 of the whole")
                      .arg(shareText(f.value / m_frames.front().value));
        QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(event);
}

void FlameGraphView::mousePressEvent(QMouseEvent *event)
{
    const int i = frameAt(event->position().toPoint());
    if (event->button() != Qt::LeftButton || i < 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_current = i;
    m_zoom = i;
    update();
    Q_EMIT nodeClicked(m_frames[std::size_t(i)].node);
}

void FlameGraphView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int i = frameAt(event->position().toPoint());
    if (i < 0) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    Q_EMIT nodeActivated(m_frames[std::size_t(i)].node);
}

} // namespace slonisko
