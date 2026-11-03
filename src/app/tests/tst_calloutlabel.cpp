// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CalloutLabel.h"

#include <QTest>

#include <cmath>

using namespace slonisko;

namespace {

QPalette palette(const char *window, const char *text, const char *base)
{
    QPalette p;
    p.setColor(QPalette::Window, QColor::fromString(QLatin1String(window)));
    p.setColor(QPalette::WindowText, QColor::fromString(QLatin1String(text)));
    p.setColor(QPalette::Base, QColor::fromString(QLatin1String(base)));
    return p;
}

// WCAG's contrast ratio, worked out here rather than taken from the code
// under test.
double contrast(const QColor &a, const QColor &b)
{
    auto luminance = [](const QColor &c) {
        auto channel = [](double v) {
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF())
            + 0.0722 * channel(c.blueF());
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

class TestCalloutLabel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readable_data()
    {
        QTest::addColumn<QPalette>("palette");
        QTest::newRow("light") << palette("#efefef", "#000000", "#ffffff");
        QTest::newRow("dark") << palette("#202020", "#ffffff", "#181818");
        // Nord: grey everywhere, which a fixed red was unreadable on.
        QTest::newRow("nord") << palette("#3b4252", "#d8dee9", "#2e3440");
        QTest::newRow("nord light") << palette("#d8dee9", "#2e3440", "#eceff4");
    }

    void readable()
    {
        QFETCH(QPalette, palette);
        QWidget parent;
        parent.setPalette(palette);
        CalloutLabel label(&parent);
        for (auto tone : {CalloutLabel::Tone::Error, CalloutLabel::Tone::Success}) {
            label.setTone(tone);
            const CalloutLabel::Colors c = label.colors();
            QVERIFY2(contrast(c.text, c.background) >= 4.5,
                     qPrintable(c.text.name() + u" on " + c.background.name()));
            QVERIFY(contrast(c.border, palette.color(QPalette::Window)) >= 3.0);
            QVERIFY(c.background != palette.color(QPalette::Window));
            QCOMPARE(label.palette().color(QPalette::WindowText), c.text);
            for (auto role : {QPalette::Window, QPalette::Base}) {
                const QColor bg = palette.color(role);
                QVERIFY2(contrast(CalloutLabel::textColor(tone, bg), bg) >= 4.5,
                         qPrintable(bg.name()));
            }
        }
    }

    void errorIsRed()
    {
        const QColor c = CalloutLabel::textColor(CalloutLabel::Tone::Error, QColor("#3b4252"));
        QVERIFY(c.red() > c.green() && c.red() > c.blue());
    }

    void followsPalette()
    {
        QWidget parent;
        parent.setPalette(palette("#3b4252", "#d8dee9", "#2e3440"));
        CalloutLabel label(&parent);
        label.setTone(CalloutLabel::Tone::Error);
        const QColor dark = label.colors().background;
        QVERIFY(dark.lightness() < 128);

        // A theme switch, as far as the label can tell.
        parent.setPalette(palette("#efefef", "#000000", "#ffffff"));
        QVERIFY(label.colors().background.lightness() > 128);
        QVERIFY(contrast(label.colors().text, label.colors().background) >= 4.5);
    }

    void plainAgain()
    {
        QWidget parent;
        parent.setPalette(palette("#3b4252", "#d8dee9", "#2e3440"));
        CalloutLabel label(&parent);
        label.setTone(CalloutLabel::Tone::Error);
        QVERIFY(label.contentsMargins().left() > 0);
        label.setError(false);
        QCOMPARE(label.tone(), CalloutLabel::Tone::Plain);
        QCOMPARE(label.palette().color(QPalette::WindowText), QColor("#d8dee9"));
        QCOMPARE(label.contentsMargins(), QMargins());
    }
};

QTEST_MAIN(TestCalloutLabel)
#include "tst_calloutlabel.moc"
