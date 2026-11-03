// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CalloutLabel.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace slonisko {

namespace {

constexpr int Accent = 4; // The thick left edge.
constexpr qreal Radius = 4;

double luminance(const QColor &c)
{
    auto channel
        = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor mix(const QColor &a, const QColor &b, double share)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * share),
                            float(a.greenF() + (b.greenF() - a.greenF()) * share),
                            float(a.blueF() + (b.blueF() - a.blueF()) * share));
}

bool isDark(const QColor &background)
{
    return background.lightness() < 128;
}

QColor accent(CalloutLabel::Tone tone, bool dark)
{
    switch (tone) {
    case CalloutLabel::Tone::Error:
        return QColor::fromString(QLatin1String(dark ? "#ff7b72" : "#c62828"));
    case CalloutLabel::Tone::Success:
        return QColor::fromString(QLatin1String(dark ? "#7ee787" : "#2e7d32"));
    case CalloutLabel::Tone::Plain:
        break;
    }
    return {};
}

// Moves colour away from background, towards white or black, until the two
// reach the contrast WCAG asks of text.
QColor readable(const QColor &color, const QColor &background, double wanted = 4.5)
{
    const QColor end = isDark(background) ? QColor(Qt::white) : QColor(Qt::black);
    QColor result = color;
    for (int step = 1; step <= 10 && contrast(result, background) < wanted; ++step)
        result = mix(color, end, step / 10.0);
    return result;
}

} // namespace

CalloutLabel::CalloutLabel(QWidget *parent) : QLabel(parent) { }

void CalloutLabel::setTone(Tone tone)
{
    if (m_tone == tone)
        return;
    m_tone = tone;
    updateColors();
}

QColor CalloutLabel::textColor(Tone tone, const QColor &background)
{
    if (tone == Tone::Plain)
        return {};
    return readable(accent(tone, isDark(background)), background);
}

void CalloutLabel::changeEvent(QEvent *event)
{
    // A new theme, or a parent's palette passed down: the tint goes with it.
    if (event->type() == QEvent::PaletteChange && !m_updating)
        updateColors();
    QLabel::changeEvent(event);
}

void CalloutLabel::updateColors()
{
    m_updating = true;
    // Back to the palette the theme and the parent give, which the text
    // colour set below would otherwise hide.
    setPalette(QPalette());
    if (m_tone == Tone::Plain) {
        m_colors = {};
        setContentsMargins(0, 0, 0, 0);
    } else {
        const QPalette theme = palette();
        const QColor window = theme.color(QPalette::Window);
        const bool dark = isDark(window);
        const QColor color = accent(m_tone, dark);
        m_colors.background = mix(window, color, dark ? 0.22 : 0.10);
        m_colors.border = readable(color, window, 3.0);
        // The theme's own text: it was chosen to read on this window colour.
        m_colors.text = readable(theme.color(QPalette::WindowText), m_colors.background);
        QPalette text;
        text.setColor(QPalette::WindowText, m_colors.text);
        setPalette(text);
        setContentsMargins(Accent + 8, 4, 8, 4);
    }
    m_updating = false;
    update();
}

void CalloutLabel::paintEvent(QPaintEvent *event)
{
    if (m_tone != Tone::Plain) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath panel;
        panel.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Radius, Radius);
        painter.fillPath(panel, m_colors.background);
        painter.setPen(QPen(m_colors.border, 1));
        painter.drawPath(panel);
        painter.setClipPath(panel);
        painter.fillRect(QRect(0, 0, Accent, height()), m_colors.border);
    }
    QLabel::paintEvent(event);
}

} // namespace slonisko
