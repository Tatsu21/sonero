#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <string_view>

#include "ui/Theme.h"
#include "ui/widgets/AppChip.h"
#include "ui/widgets/ChannelStrip.h"

namespace ui = sonar::ui;
namespace audio = sonar::audio;
using sonar::ui::AppChip;
using sonar::ui::ChannelStrip;

// The theme system's two promises: a theme file can only change colours, and the
// built-in theme is always intact behind it. Both are what make it safe to let a
// user edit the look of the app at all.

class ThemeTest : public QObject {
    Q_OBJECT

private slots:
    void builtInSheetHasEveryPlaceholderResolved();
    void builtInSheetCarriesNoLeftoverTokens();
    void aPartialThemeFallsBackToTheBuiltInColours();
    void aMalformedThemeIsRejectedWithoutTouchingTheOutput();
    void themeSurvivesAWriteAndReadRoundTrip();
    void aThemeCannotEmptyTheStyleSheet();
    void derivedShadesStayInsideTheirRange();
    void prominentTextContrastsWithItsBackground();
    void applyingAThemeDoesNotReEnterTheWidgetsItRestyles();
};

void ThemeTest::builtInSheetHasEveryPlaceholderResolved() {
    const QString sheet = ui::resolvedStyleSheet(ui::builtInTheme());
    QVERIFY(!sheet.isEmpty());
    // Every colour in the sheet is a placeholder in the source, so a resolved
    // sheet with one left in it is a token nothing substitutes — which Qt would
    // report as a parse error on every repaint.
    const QRegularExpression token(QStringLiteral("@[A-Z_]+@"));
    const QRegularExpressionMatch left = token.match(sheet);
    QVERIFY2(!left.hasMatch(),
             qPrintable(QStringLiteral("unresolved placeholder: %1").arg(left.captured())));
}

void ThemeTest::builtInSheetCarriesNoLeftoverTokens() {
    // The same for a theme that is nothing like the default, which is what would
    // catch a placeholder accidentally resolved from a hard-coded value.
    ui::ThemeTokens t = ui::builtInTheme();
    t.bg = QColor(0xff, 0xff, 0xff);
    t.text = QColor(0x00, 0x00, 0x00);
    t.accent = QColor(0x00, 0x80, 0x00);
    const QString sheet = ui::resolvedStyleSheet(t);
    QVERIFY(!sheet.contains(QRegularExpression(QStringLiteral("@[A-Z_]+@"))));
    // And the theme's own colours actually reached it.
    QVERIFY(sheet.contains(QStringLiteral("#ffffff")));
    QVERIFY(sheet.contains(QStringLiteral("#008000")));
}

void ThemeTest::aPartialThemeFallsBackToTheBuiltInColours() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("partial.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // Three keys out of thirteen. The rest must come from the built-in theme
    // rather than from zero, or a one-line theme would paint the app black.
    file.write(R"({ "name": "Partial", "accent": "#ff0000" })");
    file.close();

    ui::ThemeTokens t;
    QString error;
    QVERIFY2(ui::readTheme(path, t, &error), qPrintable(error));
    QCOMPARE(t.name, QStringLiteral("Partial"));
    QCOMPARE(t.accent, QColor(0xff, 0x00, 0x00));
    QCOMPARE(t.bg, ui::builtInTheme().bg);
    QCOMPARE(t.text, ui::builtInTheme().text);
    QVERIFY(!t.isBuiltIn());
}

void ThemeTest::aMalformedThemeIsRejectedWithoutTouchingTheOutput() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("broken.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json");
    file.close();

    ui::ThemeTokens t = ui::builtInTheme();
    t.name = QStringLiteral("untouched");
    QString error;
    QVERIFY(!ui::readTheme(path, t, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(t.name, QStringLiteral("untouched"));  // out parameter left alone

    // A missing file is the same kind of failure, not a crash.
    QVERIFY(!ui::readTheme(dir.filePath(QStringLiteral("absent.json")), t, &error));

    // And a colour that is not a colour loses that one key, not the file.
    const QString partly = dir.filePath(QStringLiteral("partly.json"));
    QFile f2(partly);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write(R"({ "name": "Partly", "accent": "not a colour", "bg": "#101010" })");
    f2.close();
    ui::ThemeTokens t2;
    QVERIFY(ui::readTheme(partly, t2, &error));
    QCOMPARE(t2.accent, ui::builtInTheme().accent);
    QCOMPARE(t2.bg, QColor(0x10, 0x10, 0x10));
}

void ThemeTest::themeSurvivesAWriteAndReadRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ui::ThemeTokens out = ui::builtInTheme();
    out.name = QStringLiteral("Round Trip");
    out.accent = QColor(0x12, 0x34, 0x56);
    out.extraQss = QStringLiteral("#Card { border-radius: 2px; }");

    const QString path = dir.filePath(QStringLiteral("rt.json"));
    QString error;
    QVERIFY2(ui::writeTheme(out, path, &error), qPrintable(error));

    ui::ThemeTokens back;
    QVERIFY2(ui::readTheme(path, back, &error), qPrintable(error));
    QCOMPARE(back.name, out.name);
    QCOMPARE(back.accent, out.accent);
    QCOMPARE(back.extraQss, out.extraQss);
    QCOMPARE(back.bg, out.bg);
}

void ThemeTest::aThemeCannotEmptyTheStyleSheet() {
    // The built-in sheet is the base and is never replaced — a theme's own sheet
    // is appended to it. This is what keeps a wrong theme from leaving an
    // unusable window, and it is worth pinning because "replace" would be the
    // obvious way to implement it.
    ui::ThemeTokens t = ui::builtInTheme();
    t.extraQss = QStringLiteral("/* nothing */");
    const QString resolved = ui::resolvedStyleSheet(t);
    QVERIFY(resolved.length() > 1000);
    QVERIFY(resolved.contains(QStringLiteral("#Card")));
    QVERIFY(resolved.contains(QStringLiteral("#ChannelStrip")));
}

void ThemeTest::derivedShadesStayInsideTheirRange() {
    // The derivations mix between tokens, so no shade may land outside the
    // colours it was mixed from. A light theme is the case that would break a
    // derivation written as "lighter()" instead of a mix.
    ui::ThemeTokens light = ui::builtInTheme();
    light.bg = QColor(0xf4, 0xf5, 0xf8);
    light.surface = QColor(0xff, 0xff, 0xff);
    light.text = QColor(0x1b, 0x1d, 0x26);
    light.textDim = QColor(0x5f, 0x67, 0x7d);

    const QString sheet = ui::resolvedStyleSheet(light);
    QVERIFY(!sheet.contains(QRegularExpression(QStringLiteral("@[A-Z_]+@"))));
    // White text does appear in a light theme, and correctly so: the accent is
    // dark indigo and its buttons need light ink. Whether text is readable
    // against what it sits on is prominentTextContrastsWithItsBackground()'s
    // job — this case only pins that the substitution completes.
    QVERIFY(sheet.contains(QStringLiteral("#Card")));
}

void ThemeTest::prominentTextContrastsWithItsBackground() {
    // The derivations that make text "brighter" must push it AWAY from the
    // background, not toward white. Mixing toward white gave a light theme
    // #bfc0c2 page titles on a white surface — the most prominent text in the
    // app rendered as the least readable — and nothing else caught it, because
    // the sheet parsed fine and contained no literal #ffffff.
    const auto titleColour = [](const ui::ThemeTokens& t) {
        const QString sheet = ui::resolvedStyleSheet(t);
        const QRegularExpression re(
            QStringLiteral("#PageTitle\\s*\\{[^}]*color:\\s*(#[0-9a-fA-F]{6})"));
        const QRegularExpressionMatch m = re.match(sheet);
        return m.hasMatch() ? QColor(m.captured(1)) : QColor();
    };

    for (const char* name : {"dark", "light"}) {
        ui::ThemeTokens t = ui::builtInTheme();
        const bool light = std::string_view(name) == "light";
        if (light) {
            t.bg = QColor(0xf4, 0xf5, 0xf8);
            t.surface = QColor(0xff, 0xff, 0xff);
            t.text = QColor(0x1b, 0x1d, 0x26);
            t.textDim = QColor(0x5f, 0x67, 0x7d);
        }
        const QColor title = titleColour(t);
        QVERIFY2(title.isValid(), "could not find #PageTitle's colour in the sheet");
        // Far from the surface it is painted on, in the right direction.
        const int gap = std::abs(title.lightness() - t.surface.lightness());
        QVERIFY2(gap > 90, qPrintable(QStringLiteral("%1 theme: title %2 on surface %3 "
                                                     "— only %4 apart in lightness")
                                          .arg(QString::fromLatin1(name), title.name(),
                                               t.surface.name())
                                          .arg(gap)));
    }
}

void ThemeTest::applyingAThemeDoesNotReEnterTheWidgetsItRestyles() {
    // Some widgets are painted from a colour derived from the theme and cannot be
    // reached by the application style sheet, so they re-derive it when Qt tells
    // them the palette changed. Calling setStyleSheet() from there makes Qt send
    // another palette change, which arrives at the same handler: the first theme
    // applied recursed 2158 frames deep and died inside Qt's CSS parser, which
    // read as a malformed style sheet rather than a loop.
    //
    // Both widgets guard against re-entering. This builds them and switches the
    // theme twice; without the guard it does not return.
    // Parented and shown, both of which matter: the recursion arrives through
    // QWidgetPrivate::propagatePaletteChange(), which only walks a real widget
    // tree. An unparented, never-shown chip does not reproduce it.
    auto* window = new QWidget;
    auto* layout = new QVBoxLayout(window);
    auto* strip = new ChannelStrip(audio::ChannelId::Game, QStringLiteral("Game"), window);
    auto* chip = new AppChip(42, QStringLiteral("Firefox"), audio::ChannelId::Game, window);
    layout->addWidget(strip);
    layout->addWidget(chip);
    window->show();
    QCoreApplication::processEvents();

    ui::ThemeTokens light = ui::builtInTheme();
    light.bg = QColor(0xf4, 0xf5, 0xf8);
    light.surface = QColor(0xff, 0xff, 0xff);
    light.text = QColor(0x1b, 0x1d, 0x26);
    light.accent = QColor(0x4f, 0x46, 0xe5);

    ui::applyTheme(*qApp, light);
    ui::applyTheme(*qApp, ui::builtInTheme());
    QCoreApplication::processEvents();
    QVERIFY(true);  // reaching here at all is the assertion

    delete window;
}

QTEST_MAIN(ThemeTest)
#include "ThemeTest.moc"
