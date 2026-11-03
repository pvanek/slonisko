// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QLabel>

namespace slonisko {

// A label that shows an error or a success as a tinted panel. A fixed red
// that works on white is unreadable on Nord's grey, so every colour is worked
// out from the palette, and again whenever the palette changes.
class CalloutLabel : public QLabel
{
    Q_OBJECT
    Q_PROPERTY(Tone tone READ tone WRITE setTone)

public:
    enum class Tone { Plain, Error, Success };
    Q_ENUM(Tone)

    struct Colors
    {
        QColor background;
        QColor border;
        QColor text;
    };

    explicit CalloutLabel(QWidget *parent = nullptr);

    Tone tone() const { return m_tone; }
    void setTone(Tone tone);
    void setError(bool error) { setTone(error ? Tone::Error : Tone::Plain); }
    // What the panel is drawn with; nothing for Tone::Plain.
    Colors colors() const { return m_colors; }

    // The tone's colour for text drawn straight on background, where a panel
    // would be too much: a line in a log, a status in a toolbar.
    static QColor textColor(Tone tone, const QColor &background);

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateColors();

    Tone m_tone = Tone::Plain;
    Colors m_colors;
    bool m_updating = false;
};

} // namespace slonisko
