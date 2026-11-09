#include "CustomPaletteTest.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QScopeGuard>

#include "CustomPlugin.h"
#include "QGCPalette.h"

void CustomPaletteTest::_paletteContract_data()
{
    QTest::addColumn<int>("theme");
    QTest::addColumn<bool>("enabled");

    QTest::newRow("light-enabled") << int(QGCPalette::Light) << true;
    QTest::newRow("light-disabled") << int(QGCPalette::Light) << false;
    QTest::newRow("dark-enabled") << int(QGCPalette::Dark) << true;
    QTest::newRow("dark-disabled") << int(QGCPalette::Dark) << false;
}

void CustomPaletteTest::_paletteContract()
{
    QFETCH(int, theme);
    QFETCH(bool, enabled);

    QGCCorePlugin* plugin = QGCCorePlugin::instance();
    QVERIFY(qobject_cast<CustomPlugin*>(plugin));

    const QStringList removedRoles = {
        QStringLiteral("windowShade"),
        QStringLiteral("windowShadeDark"),
        QStringLiteral("windowShadeLight"),
        QStringLiteral("text"),
        QStringLiteral("button"),
        QStringLiteral("buttonBorder"),
        QStringLiteral("buttonText"),
        QStringLiteral("buttonHighlight"),
        QStringLiteral("buttonHighlightText"),
        QStringLiteral("primaryButton"),
        QStringLiteral("primaryButtonText"),
        QStringLiteral("textField"),
        QStringLiteral("textFieldText"),
        QStringLiteral("textFieldErrorText"),
        QStringLiteral("groupBorder"),
    };
    const QColor sentinel("#123456");
    const int group = enabled ? QGCPalette::ColorGroupEnabled : QGCPalette::ColorGroupDisabled;
    for (const QString& role : removedRoles) {
        QGCPalette::PaletteColorInfo_t colors = {{sentinel, sentinel}, {sentinel, sentinel}};
        plugin->paletteOverride(role, colors);
        QVERIFY2(colors[theme][group] == sentinel, qPrintable(role));
    }

    const auto previousTheme = QGCPalette::globalTheme();
    const auto restoreTheme = qScopeGuard([previousTheme]() { QGCPalette::setGlobalTheme(previousTheme); });
    QGCPalette::setGlobalTheme(static_cast<QGCPalette::Theme>(theme));
    // QGCPalette builds its map through the active core plugin and retains defaults for unoverridden roles.
    QGCPalette palette;
    palette.setColorGroupEnabled(enabled);
    for (const QString& role : removedRoles) {
        QVERIFY2(!palette.colors().contains(role), qPrintable(role));
    }

    const auto luminance = [](const QColor& color) {
        const auto linear = [](double channel) {
            return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
    };
    const auto verifyContrast = [&palette, &luminance](const char* foreground, const char* background, double minimum) {
        const QColor fg = palette.property(foreground).value<QColor>();
        const QColor bg = palette.property(background).value<QColor>();
        QVERIFY(fg.isValid() && bg.isValid());
        QCOMPARE(fg.alpha(), 255);
        QCOMPARE(bg.alpha(), 255);
        const double fgLuminance = luminance(fg);
        const double bgLuminance = luminance(bg);
        const double contrast =
            (std::max(fgLuminance, bgLuminance) + 0.05) / (std::min(fgLuminance, bgLuminance) + 0.05);
        const QString message = QStringLiteral("%1 on %2: %3 < %4")
                                    .arg(QString::fromLatin1(foreground), QString::fromLatin1(background))
                                    .arg(contrast, 0, 'f', 3)
                                    .arg(minimum);
        QVERIFY2(contrast >= minimum, qPrintable(message));
    };

    for (const char* surface : {"window", "windowPanel", "windowCard", "windowInset"}) {
        verifyContrast("textPrimary", surface, 4.5);
    }
    if (enabled) {
        verifyContrast("accentText", "accent", 3.0);
    }
    verifyContrast("errorText", "window", 3.0);
    verifyContrast("warningText", "window", 3.0);
    verifyContrast("accent", "windowCard", 1.2);
}

UT_REGISTER_TEST(CustomPaletteTest, TestLabel::Unit)
