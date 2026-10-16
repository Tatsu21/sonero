#include "ui/Theme.h"

#include <QApplication>
#include <QDir>

#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStyle>
#include <QVariant>
#include <QWidget>

#include "app/SystemSetup.h"

namespace sonar::ui {

namespace {

// The built-in theme's style sheet. Colours are placeholders rather than
// literals: every one is filled in from ThemeTokens at apply time, which is what
// makes the whole interface themeable without a user having to write any of this.
//
// Thirteen of the placeholders are the theme's own colours. The rest are shades
// derived from them in derive() below — a hover a little lighter than its
// surface, a border a little lighter than that — so a theme file carries thirteen
// decisions instead of thirty-six.
//
// This string is the baseline and is never replaced, only substituted into. A
// theme's own `extraQss` is appended after it, so it wins where the two overlap
// and the app stays usable when it is wrong.
const char* const kStyleSheet = R"QSS(
* {
    font-family: @FONT@;
    outline: 0;
}

QWidget { background: @BG@; color: @TEXT@; font-size: 13px; }

/* Text/controls must be transparent so cards show through (no boxed text). */
QLabel, QCheckBox, QRadioButton, QGroupBox { background: transparent; }
QToolTip {
    background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER@;
    border-radius: 8px; padding: 6px 9px;
}

/* ---- Top bar ---- */
/* No padding: the wordmark is centred by the layout, and any asymmetric padding
   would push it off the mark's and the nav pills' shared centre line. */
#AppLogo { font-size: 18px; font-weight: 800; padding: 0; }

/* ---- Page content ---- */
#PageTitle { font-size: 25px; font-weight: 800; color: @TEXT_STRONG@; }
#PageSubtitle { color: @TEXT_DIM@; font-size: 13px; }
#SectionTitle { font-size: 15px; font-weight: 700; color: @TEXT_BRIGHT@; }
#Hint { color: @TEXT_HINT@; }
#Sep { background: @SEP@; max-height: 1px; min-height: 1px; border: none; }

/* ---- Cards ---- */
#Card, #ChannelStrip {
    background: @SURFACE@; border: 1px solid @BORDER_SOFT@; border-radius: 16px;
}
#Card:hover { border-color: @BORDER_HOVER@; }
#ChannelStrip[dropActive="true"] {
    background: @SURFACE_ACCENT@; border: 1px solid @ACCENT@;
}
#StatusValueOk { color: @OK@; font-weight: 800; }
#StatusValueBad { color: @DANGER@; font-weight: 800; }
#CardKey { color: @TEXT_DIM@; }
#CardVal { color: @TEXT@; font-weight: 600; }

/* ---- Channel strip ---- */
#ChannelName { font-weight: 700; color: @TEXT_STRONG@; font-size: 13px; }
#VolumeValue { color: @TEXT_SOFT@; font-size: 13px; font-weight: 700; }
#BalanceCaption { color: @TEXT_DISABLED@; font-size: 10px; font-weight: 700; letter-spacing: 1px; }
#AppsLabel { color: @ACCENT_HI@; font-size: 10px; font-weight: 600; }

/* ---- Application chips (draggable) ---- */
#AppChip {
    background: @INPUT@; border: 1px solid @BORDER_STRONG@; border-radius: 17px;
}
#AppChip:hover { background: @HOVER@; border-color: @GROOVE@; }
#ChipName { color: @TEXT@; font-weight: 600; }
#ChipBadge { color: @ACCENT_HI@; font-weight: 700; }
#ChipDot { color: @TEXT_DISABLED@; }

/* ---- Microphone page ---- */
#SoonBadge {
    background: @INPUT_RAISED@; color: @ACCENT_HI@; font-size: 9px; font-weight: 800;
    border-radius: 6px; padding: 3px 8px;
}
#MicLevel {
    background: @BG_RAISED@; border: 1px solid @BORDER_SOFT@; border-radius: 6px;
    min-height: 14px; max-height: 14px;
}
#MicLevel::chunk {
    border-radius: 5px; margin: 0px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 @OK@, stop:0.7 @OK@, stop:0.85 @WARN@, stop:1 @DANGER@);
}
#BatteryBar {
    background: @BG_RAISED@; border: 1px solid @BORDER_SOFT@; border-radius: 6px;
    min-height: 14px; max-height: 14px;
}
#BatteryBar::chunk {
    border-radius: 5px; margin: 0px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @OK@, stop:1 @OK_HI@);
}

/* ---- Segmented control ---- */
#Segmented { background: @BG_RAISED@; border: 1px solid @BORDER_SOFT@; border-radius: 11px; }
#Segmented QPushButton {
    background: transparent; border: none; border-radius: 8px;
    padding: 7px 14px; color: @TEXT_DIM@; font-weight: 600;
}
#Segmented QPushButton:hover { color: @TEXT@; }
#Segmented QPushButton:checked { background: @ACCENT@; color: @ON_ACCENT@; }

/* ---- Buttons ---- */
QPushButton {
    background: @INPUT@; color: @TEXT@; border: 1px solid @BORDER_STRONG@;
    border-radius: 10px; padding: 8px 14px; font-weight: 600;
}
QPushButton:hover { background: @HOVER@; border-color: @GROOVE@; }
QPushButton:pressed { background: @SURFACE@; }
QPushButton#Accent { background: @ACCENT@; color: @ON_ACCENT@; border: none; }
QPushButton#Accent:hover { background: @ACCENT_MID@; }

QPushButton[toggleRole="mute"], QPushButton[toggleRole="solo"] {
    padding: 8px 4px; font-size: 12px;
}
QPushButton[toggleRole="mute"]:checked {
    background: @DANGER@; color: @ON_DANGER@; border-color: @DANGER@; font-weight: 800;
}
QPushButton[toggleRole="solo"]:checked {
    background: @WARN@; color: @ON_WARN@; border-color: @WARN@; font-weight: 800;
}

/* ---- Sliders ---- */
QSlider::groove:horizontal { height: 6px; background: @BORDER@; border-radius: 3px; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 3px; }
QSlider::handle:horizontal {
    background: @TEXT_STRONG@; width: 15px; height: 15px; margin: -6px 0;
    border-radius: 8px; border: 2px solid @ACCENT@;
}
QSlider::handle:horizontal:hover { border-color: @ACCENT_HI@; }

QSlider::groove:vertical { width: 7px; background: @BORDER@; border-radius: 4px; }
QSlider::add-page:vertical { background: @ACCENT@; border-radius: 4px; }
QSlider::sub-page:vertical { background: @BORDER@; border-radius: 4px; }
QSlider::handle:vertical {
    background: @TEXT_STRONG@; height: 17px; width: 17px; margin: 0 -6px;
    border-radius: 9px; border: 2px solid @ACCENT@;
}
QSlider::handle:vertical:hover { border-color: @ACCENT_HI@; }

/* ---- Combo box ---- */
QComboBox {
    background: @INPUT@; color: @TEXT@; border: 1px solid @BORDER_STRONG@;
    border-radius: 10px; padding: 7px 12px; min-height: 18px;
}
QComboBox:hover { border-color: @ACCENT@; }
QComboBox::drop-down { border: none; width: 24px; }
/* An image, because Qt leaves nothing else. The usual CSS triangle — zero size
   plus transparent side borders — is a web trick Qt does not implement: it draws
   the sub-control's box, so all that appeared was the top border, a small solid
   dash where the arrow should be. Dropping the rule does not help either: once a
   stylesheet touches a combo box, the stylesheet draws it, and with no image
   there is simply no arrow. The paths are filled in by applyTheme(). */
QComboBox::down-arrow {
    image: url("@ARROW@"); width: 12px; height: 12px; margin-right: 8px;
}
QComboBox::down-arrow:disabled { image: url("@ARROW_DISABLED@"); }
QComboBox QAbstractItemView {
    background: @SURFACE@; border: 1px solid @BORDER@; border-radius: 10px;
    selection-background-color: @HOVER@; color: @TEXT@; padding: 6px;
}
QComboBox QAbstractItemView::item { padding: 7px 10px; border-radius: 7px; min-height: 20px; }

/* ---- Check box ---- */
QCheckBox { color: @TEXT@; spacing: 8px; }
QCheckBox::indicator {
    width: 18px; height: 18px; border-radius: 6px;
    border: 1px solid @BORDER_STRONG@; background: @INPUT@;
}
QCheckBox::indicator:hover { border-color: @ACCENT@; }
QCheckBox::indicator:checked { background: @ACCENT@; border-color: @ACCENT@; }

/* ---- Disabled: present in the UI, not yet wired to anything ---- */
/* Qt's disabled palette never reaches these widgets — the rules above set their
   colours outright — so every inert control needs its own grey here. One grey
   for all of them, so "you cannot touch this" reads the same on a button, a
   slider and a checkbox. */
QPushButton:disabled { background: @BG_RAISED@; color: @HANDLE@; border-color: @INPUT_RAISED@; }
QSlider::groove:horizontal:disabled,
QSlider::groove:vertical:disabled,
QSlider::sub-page:horizontal:disabled,
QSlider::sub-page:vertical:disabled,
QSlider::add-page:vertical:disabled { background: @INPUT_RAISED@; }
QSlider::handle:horizontal:disabled,
QSlider::handle:vertical:disabled { background: @BORDER_HOVER@; border-color: @INPUT_RAISED@; }
QComboBox:disabled { background: @BG_RAISED@; color: @HANDLE@; border-color: @INPUT_RAISED@; }
QCheckBox:disabled { color: @HANDLE@; }
QCheckBox::indicator:disabled { background: @BG_RAISED@; border-color: @INPUT_RAISED@; }
#MicLevel::chunk:disabled { background: @BORDER_STRONG@; }
/* An id selector outranks QLabel:disabled, so every named label needs its own
   disabled colour or it stays bright next to the control it belongs to. */
QLabel:disabled { color: @HANDLE@; }
#CardKey:disabled, #CardVal:disabled, #VolumeValue:disabled,
#BalanceCaption:disabled, #ChannelName:disabled { color: @HANDLE@; }

/* ---- Scroll bars ---- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @BORDER@; border-radius: 5px; min-height: 32px; }
QScrollBar::handle:vertical:hover { background: @GROOVE@; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @BORDER@; border-radius: 5px; min-width: 32px; }
QScrollBar::handle:horizontal:hover { background: @GROOVE@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---- Window chrome ---- */
/* The panel the pages sit on, and the row of nav pills above it. Both used to
   set their own sheets from MainWindow, which meant a theme change could not
   reach them — a widget carrying its own style sheet is immune to the
   application's. The pills' per-page tints are generated into this sheet from
   the theme, which is why they are here and not a literal palette. */
#ContentPanel { background: @PANEL@; border-radius: 16px; }
#NavPill {
    border: none; border-radius: 9px; padding: 0 18px; font-size: 14px;
    color: @TEXT_SOFT@;
}
#NavPill:hover, #NavPill:checked { color: @TEXT_STRONG@; }
#NavPill:checked { font-weight: 700; }
#NavPill:disabled { background: @BG_RAISED@; color: @TEXT_DISABLED@; }
@NAVTINTS@

/* ---- Tray volume window ---- */
/* Styled here rather than in TrayMixer.cpp: a widget that sets its own sheet is
   immune to the application sheet, and so immune to the theme. */
#TrayMixer { background: @BG_RAISED@; }
#TrayMixer QLabel { background: transparent; }
#TrayMixerName { color: @TEXT_SOFT@; font-size: 13px; font-weight: 600; }
#TrayMixerValue { color: @TEXT_DIM@; font-size: 12px; font-weight: 700; }
#TrayMixerTag {
    color: @ACCENT_HI@; font-size: 9px; font-weight: 800; letter-spacing: 1px;
}
#TrayMixerRule { background: @BORDER_STRONG@; max-height: 1px; min-height: 1px; }

/* ---- State colours, selected by property so a theme change repaints them ---- */
/* The alternative was an inline sheet per label, which cannot follow a theme. */
QLabel[sonerostate="ok"] { color: @OK@; font-weight: 800; }
QLabel[sonerostate="warn"] { color: @WARN@; font-weight: 800; }
QLabel[sonerostate="bad"] { color: @DANGER@; font-weight: 800; }

/* ---- Monospace blocks (paths, logs, the update changelog) ---- */
#CodeBlock {
    font-family: monospace; background: @BG_RAISED@; border-radius: 6px;
    padding: 8px 10px; color: @TEXT_DIM@;
}
#InsetRow { background: @BG_RAISED@; border-radius: 8px; }
)QSS";

// --- Colour derivation -------------------------------------------------------

// How bright a colour looks, 0..255. Not QColor::lightness(), which is HSL and
// averages the extremes: it rates cyan #05d9e8 at 46% and so would put white
// text on it. The green-weighted form is what the eye actually does, and it is
// what decides every light-or-dark choice below.
[[nodiscard]] int luma(const QColor& c) {
    return (299 * c.red() + 587 * c.green() + 114 * c.blue()) / 1000;
}

// Mix two colours. Straight linear interpolation in sRGB, which is not
// perceptually even but is what every hand-picked value in the original palette
// was eyeballed against — matching it matters more here than being correct.
[[nodiscard]] QColor mix(const QColor& a, const QColor& b, double t) {
    const double u = 1.0 - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t, a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t);
}

// Every placeholder the sheet uses, resolved against one theme. The derivations
// reproduce the original hand-picked palette closely enough to be invisible, and
// they are what let a thirteen-colour theme file paint every surface.
[[nodiscard]] QHash<QString, QString> derive(const ThemeTokens& t) {
    const QColor white(0xff, 0xff, 0xff);
    const QColor black(0x0a, 0x0b, 0x10);

    // Whichever of the two the background is furthest from. Everything that has
    // to stay legible against the background is pushed toward this.
    const QColor contrast = luma(t.bg) < 128 ? white : black;

    // Ink that sits ON a strong colour is a different question from contrasting
    // with the background: a light theme can perfectly well have a dark accent,
    // and then its buttons need light text while its titles need dark.
    const auto ink = [&](const QColor& on) { return luma(on) < 128 ? white : black; };

    QHash<QString, QString> v;
    const auto put = [&v](const char* key, const QColor& c) {
        v.insert(QString::fromLatin1(key), c.name(QColor::HexRgb));
    };

    // The thirteen a theme actually carries.
    put("@BG@", t.bg);
    put("@SIDEBAR@", t.sidebar);
    put("@SURFACE@", t.surface);
    put("@INPUT@", t.input);
    put("@HOVER@", t.hover);
    put("@BORDER@", t.border);
    put("@TEXT@", t.text);
    put("@TEXT_DIM@", t.textDim);
    put("@ACCENT@", t.accent);
    put("@ACCENT_HI@", t.accentHi);
    put("@DANGER@", t.danger);
    put("@WARN@", t.warn);
    put("@OK@", t.ok);

    // ...and the shades between them.
    put("@BG_RAISED@", mix(t.bg, t.surface, 0.45));
    put("@SURFACE_ACCENT@", mix(t.surface, t.accent, 0.18));
    put("@SEP@", mix(t.bg, t.border, 0.45));
    put("@INPUT_RAISED@", mix(t.input, t.hover, 0.45));
    put("@BORDER_SOFT@", mix(t.surface, t.border, 0.72));
    put("@BORDER_STRONG@", mix(t.border, t.hover, 0.55));
    put("@BORDER_HOVER@", mix(t.border, t.accent, 0.28));
    put("@GROOVE@", mix(t.border, t.textDim, 0.28));
    put("@HANDLE@", mix(t.border, t.textDim, 0.52));
    put("@TEXT_DISABLED@", mix(t.textDim, t.bg, 0.42));
    put("@TEXT_HINT@", mix(t.textDim, t.bg, 0.22));
    put("@TEXT_SOFT@", mix(t.text, t.textDim, 0.45));
    // Pushed AWAY from the background, not toward white. A light theme has dark
    // text, so "brighter" there means darker — mixing toward white gave #bfc0c2
    // page titles on a white surface, which is the most prominent text in the app
    // rendered as the least readable.
    put("@TEXT_BRIGHT@", mix(t.text, contrast, 0.25));
    put("@TEXT_STRONG@", mix(t.text, contrast, 0.72));
    put("@ACCENT_MID@", mix(t.accent, t.accentHi, 0.5));
    put("@OK_HI@", mix(t.ok, contrast, 0.22));

    put("@PANEL@", mix(t.surface, t.hover, 0.55));
    put("@ON_ACCENT@", ink(t.accent));
    put("@ON_DANGER@", ink(t.danger));
    put("@ON_WARN@", ink(t.warn));

    // One faint tint per page, so the eye can find a page by colour before
    // reading the word. The hues are the pages' identity; saturation and
    // lightness come from the theme, so the row follows it instead of staying
    // put — and generating the rules here is what lets that happen at all.
    static constexpr int kNavHues[] = {240, 142, 268, 320, 38, 187, 219};
    QString tints;
    for (int i = 0; i < 7; ++i) {
        const QColor tint = QColor::fromHsl(
            kNavHues[i], std::clamp(t.accent.hslSaturation() / 5, 12, 90),
            std::clamp(t.surface.lightness() + 6, 0, 255));
        tints += QStringLiteral("#NavPill[tab=\"%1\"] { background: %2; }\n")
                     .arg(i)
                     .arg(tint.name(QColor::HexRgb));
    }
    v.insert(QStringLiteral("@NAVTINTS@"), tints);

    v.insert(QStringLiteral("@FONT@"), t.fontFamily);
    return v;
}

// The theme in force. Default-initialised to the built-in one so that a widget
// painting itself before applyTheme() has run still gets sensible colours.
ThemeTokens g_current;

// --- Theme files -------------------------------------------------------------

[[nodiscard]] QString colorKey(const QColor& c) { return c.name(QColor::HexRgb); }

void writeColor(QJsonObject& o, const char* key, const QColor& c) {
    o[QString::fromLatin1(key)] = colorKey(c);
}

// Reads one colour, leaving the fallback in place if the value is missing or not
// a colour Qt recognises. A theme with one bad line loses that line, not the
// whole file.
void readColor(const QJsonObject& o, const char* key, QColor& into) {
    const QJsonValue v = o.value(QString::fromLatin1(key));
    if (!v.isString()) {
        return;
    }
    const QColor parsed(v.toString());
    if (parsed.isValid()) {
        into = parsed;
    }
}

}  // namespace

ThemeTokens builtInTheme() { return ThemeTokens{}; }

const ThemeTokens& currentTheme() { return g_current; }

QString resolvedStyleSheet(const ThemeTokens& theme) {
    QString sheet = QString::fromUtf8(kStyleSheet);
    const QHash<QString, QString> values = derive(theme);
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        sheet.replace(it.key(), it.value());
    }

    // The combo box arrow is a file, and where that file lives depends on how
    // Sonero was installed — an AppImage mount, a system prefix, or the source
    // tree. resourcePath() knows all three. If it is missing, the placeholders
    // are dropped rather than left in the sheet: a url() pointing at "@ARROW@"
    // makes Qt log a warning for every combo box on every repaint.
    const QString arrow = setup::resourcePath(QStringLiteral("icons/combo-arrow.png"));
    const QString arrowOff =
        setup::resourcePath(QStringLiteral("icons/combo-arrow-disabled.png"));
    if (arrow.isEmpty() || arrowOff.isEmpty()) {
        sheet.remove(QRegularExpression(QStringLiteral("QComboBox::down-arrow[^}]*\\}")));
    } else {
        sheet.replace(QStringLiteral("@ARROW@"), arrow);
        sheet.replace(QStringLiteral("@ARROW_DISABLED@"), arrowOff);
    }
    return sheet;
}

void applyTheme(QApplication& app, const ThemeTokens& theme) {
    g_current = theme;
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // The palette matters for what the style sheet does not reach: native menus,
    // the tray's own context menu, and whatever a Qt widget draws from the
    // palette rather than from a stylable property.
    const ThemeTokens& t = g_current;
    const QColor disabled = mix(t.textDim, t.bg, 0.42);
    QPalette p;
    p.setColor(QPalette::Window, t.bg);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.input);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.input);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, mix(t.text, QColor(0xff, 0xff, 0xff), 0.72));
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, mix(t.text, QColor(0xff, 0xff, 0xff), 0.72));
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textDim);
    // Rich-text links take their colour from the palette, which is why the
    // anchors in this app carry no inline style of their own — an inline
    // colour would be the one thing a theme could not reach.
    p.setColor(QPalette::Link, t.accentHi);
    p.setColor(QPalette::LinkVisited, t.accent);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    app.setPalette(p);

    QString sheet = resolvedStyleSheet(g_current);
    if (!g_current.extraQss.isEmpty()) {
        // Appended, not merged: later rules win in Qt, so a theme can override
        // anything above without having to reproduce it.
        sheet += QLatin1Char('\n');
        sheet += g_current.extraQss;
    }
    app.setStyleSheet(sheet);
}

void applyTheme(QApplication& app) { applyTheme(app, builtInTheme()); }

void refreshTheme(QApplication& app) {
    applyTheme(app, g_current);
    // Qt re-polishes widgets on a style sheet change, but the custom-painted ones
    // read currentTheme() in paintEvent and have no way to know it moved.
    for (QWidget* w : app.allWidgets()) {
        w->update();
    }
}

void setState(QWidget* widget, const char* state) {
    if (widget == nullptr) {
        return;
    }
    widget->setProperty("sonerostate",
                        state != nullptr ? QVariant(QString::fromLatin1(state))
                                         : QVariant());
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// --- Theme files -------------------------------------------------------------

QString themesDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/themes");
}

bool readTheme(const QString& path, ThemeTokens& out, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot be read: %1").arg(file.errorString()));
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("is not valid JSON: %1").arg(parse.errorString()));
    }
    const QJsonObject o = doc.object();

    // Built on top of the built-in theme rather than on zeroes, so a file that
    // names three colours gets the other ten from the baseline instead of black.
    ThemeTokens t = builtInTheme();
    if (o.value(QStringLiteral("name")).isString()) {
        t.name = o.value(QStringLiteral("name")).toString();
    } else {
        t.name = QFileInfo(path).completeBaseName();
    }
    readColor(o, "bg", t.bg);
    readColor(o, "sidebar", t.sidebar);
    readColor(o, "surface", t.surface);
    readColor(o, "input", t.input);
    readColor(o, "hover", t.hover);
    readColor(o, "border", t.border);
    readColor(o, "text", t.text);
    readColor(o, "textDim", t.textDim);
    readColor(o, "accent", t.accent);
    readColor(o, "accentHi", t.accentHi);
    readColor(o, "danger", t.danger);
    readColor(o, "warn", t.warn);
    readColor(o, "ok", t.ok);
    if (o.value(QStringLiteral("font")).isString()) {
        t.fontFamily = o.value(QStringLiteral("font")).toString();
    }
    if (o.value(QStringLiteral("qss")).isString()) {
        t.extraQss = o.value(QStringLiteral("qss")).toString();
    }
    t.filePath = path;
    out = t;
    return true;
}

bool writeTheme(const ThemeTokens& theme, const QString& path, QString* error) {
    QJsonObject o;
    o[QStringLiteral("name")] = theme.name;
    writeColor(o, "bg", theme.bg);
    writeColor(o, "sidebar", theme.sidebar);
    writeColor(o, "surface", theme.surface);
    writeColor(o, "input", theme.input);
    writeColor(o, "hover", theme.hover);
    writeColor(o, "border", theme.border);
    writeColor(o, "text", theme.text);
    writeColor(o, "textDim", theme.textDim);
    writeColor(o, "accent", theme.accent);
    writeColor(o, "accentHi", theme.accentHi);
    writeColor(o, "danger", theme.danger);
    writeColor(o, "warn", theme.warn);
    writeColor(o, "ok", theme.ok);
    o[QStringLiteral("font")] = theme.fontFamily;
    o[QStringLiteral("qss")] = theme.extraQss;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

QVector<ThemeTokens> availableThemes() {
    QVector<ThemeTokens> themes;
    themes.push_back(builtInTheme());  // always first, always present

    // The user's own themes, then the examples that ship with the install — so a
    // fresh install already offers a choice, and a user theme of the same name
    // shadows the example rather than the other way round.
    QStringList seen;
    for (const QString& where : {themesDirectory(),
                                 setup::resourcePath(QStringLiteral("themes"))}) {
        if (where.isEmpty()) {
            continue;
        }
        QDir dir(where);
        const QStringList files =
            dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : files) {
            if (seen.contains(name)) {
                continue;
            }
            ThemeTokens t;
            // A broken file is skipped, not reported: this is a list for a combo
            // box, and failing it would hide every working theme behind one bad
            // one.
            if (readTheme(dir.absoluteFilePath(name), t)) {
                seen.push_back(name);
                themes.push_back(t);
            }
        }
    }
    return themes;
}

QString importTheme(const QString& sourcePath, QString* error) {
    ThemeTokens t;
    if (!readTheme(sourcePath, t, error)) {
        return {};
    }
    const QString dir = themesDirectory();
    QDir().mkpath(dir);

    // Keep the source's name, but never overwrite a theme already there.
    const QString base = QFileInfo(sourcePath).completeBaseName();
    QString target = dir + QLatin1Char('/') + base + QStringLiteral(".json");
    for (int n = 2; QFile::exists(target); ++n) {
        target = dir + QLatin1Char('/') + base + QStringLiteral(" (%1).json").arg(n);
    }
    if (!writeTheme(t, target, error)) {
        return {};
    }
    return target;
}

}  // namespace sonar::ui
