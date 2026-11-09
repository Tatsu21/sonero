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

// The built-in style sheet. Colour placeholders are filled from ThemeTokens (the
// thirteen colours plus shades from derive()); a theme's `extraQss` follows it.
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

// Perceived brightness 0..255 (green-weighted), not HSL lightness, which would put
// white text on cyan.
[[nodiscard]] int luma(const QColor& c) {
    return (299 * c.red() + 587 * c.green() + 114 * c.blue()) / 1000;
}


// The thirteen colours a theme file carries: its JSON key, the style sheet
// placeholder it fills, and where it lives in ThemeTokens.
struct ColorSlot {
    const char* key;
    const char* placeholder;
    QColor ThemeTokens::*member;
};
constexpr ColorSlot kColorSlots[] = {
    {"bg", "@BG@", &ThemeTokens::bg},
    {"sidebar", "@SIDEBAR@", &ThemeTokens::sidebar},
    {"surface", "@SURFACE@", &ThemeTokens::surface},
    {"input", "@INPUT@", &ThemeTokens::input},
    {"hover", "@HOVER@", &ThemeTokens::hover},
    {"border", "@BORDER@", &ThemeTokens::border},
    {"text", "@TEXT@", &ThemeTokens::text},
    {"textDim", "@TEXT_DIM@", &ThemeTokens::textDim},
    {"accent", "@ACCENT@", &ThemeTokens::accent},
    {"accentHi", "@ACCENT_HI@", &ThemeTokens::accentHi},
    {"danger", "@DANGER@", &ThemeTokens::danger},
    {"warn", "@WARN@", &ThemeTokens::warn},
    {"ok", "@OK@", &ThemeTokens::ok},
};

// Every placeholder, resolved for one theme; the derivations reproduce the
// original hand-picked palette.
[[nodiscard]] QHash<QString, QString> derive(const ThemeTokens& t) {
    const QColor white(0xff, 0xff, 0xff);
    const QColor black(0x0a, 0x0b, 0x10);

    // Whichever of the two the background is furthest from. Everything that has
    // to stay legible against the background is pushed toward this.
    const QColor contrast = luma(t.bg) < 128 ? white : black;

    // Ink on a strong colour is decided by that colour, not by the background.
    const auto ink = [&](const QColor& on) { return luma(on) < 128 ? white : black; };

    QHash<QString, QString> v;
    const auto put = [&v](const char* key, const QColor& c) {
        v.insert(QString::fromLatin1(key), c.name(QColor::HexRgb));
    };

    // The thirteen a theme actually carries.
    for (const ColorSlot& slot : kColorSlots) {
        put(slot.placeholder, t.*slot.member);
    }

    // ...and the shades between them.
    put("@BG_RAISED@", mixColors(t.bg, t.surface, 0.45));
    put("@SURFACE_ACCENT@", mixColors(t.surface, t.accent, 0.18));
    put("@SEP@", mixColors(t.bg, t.border, 0.45));
    put("@INPUT_RAISED@", mixColors(t.input, t.hover, 0.45));
    put("@BORDER_SOFT@", mixColors(t.surface, t.border, 0.72));
    put("@BORDER_STRONG@", mixColors(t.border, t.hover, 0.55));
    put("@BORDER_HOVER@", mixColors(t.border, t.accent, 0.28));
    put("@GROOVE@", mixColors(t.border, t.textDim, 0.28));
    put("@HANDLE@", mixColors(t.border, t.textDim, 0.52));
    put("@TEXT_DISABLED@", mixColors(t.textDim, t.bg, 0.42));
    put("@TEXT_HINT@", mixColors(t.textDim, t.bg, 0.22));
    put("@TEXT_SOFT@", mixColors(t.text, t.textDim, 0.45));
    // Pushed away from the background, not toward white: on a light theme "brighter"
    // means darker.
    put("@TEXT_BRIGHT@", mixColors(t.text, contrast, 0.25));
    put("@TEXT_STRONG@", mixColors(t.text, contrast, 0.72));
    put("@ACCENT_MID@", mixColors(t.accent, t.accentHi, 0.5));
    put("@OK_HI@", mixColors(t.ok, contrast, 0.22));

    put("@PANEL@", mixColors(t.surface, t.hover, 0.55));
    put("@ON_ACCENT@", ink(t.accent));
    put("@ON_DANGER@", ink(t.danger));
    put("@ON_WARN@", ink(t.warn));

    // One faint tint per page: the hues are the pages' identity, saturation and
    // lightness follow the theme.
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

// One colour, keeping the fallback when missing or invalid: a bad line loses only itself.
void readColor(const QJsonObject& o, const char* key, QColor& into) {
    const QJsonValue v = o.value(QString::fromLatin1(key));
    if (!v.isString()) return;
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

    // The combo arrow is a file whose location depends on the install. If missing, the
    // rule is dropped, since a url("@ARROW@") makes Qt warn on every repaint.
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

    // The palette covers what the style sheet cannot reach (native and tray menus).
    const ThemeTokens& t = g_current;
    const QColor disabled = mixColors(t.textDim, t.bg, 0.42);
    QPalette p;
    p.setColor(QPalette::Window, t.bg);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.input);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.input);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, mixColors(t.text, QColor(0xff, 0xff, 0xff), 0.72));
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, mixColors(t.text, QColor(0xff, 0xff, 0xff), 0.72));
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textDim);
    // Links take their colour from the palette, so anchors carry no inline style.
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

QColor mixColors(const QColor& a, const QColor& b, double t) {
    const double u = 1.0 - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t, a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t);
}

void setState(QWidget* widget, const char* state) {
    if (widget == nullptr) return;
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

QString newThemePath(const QString& baseName) {
    const QString dir = themesDirectory();
    QDir().mkpath(dir);
    QString path = dir + QLatin1Char('/') + baseName + QStringLiteral(".json");
    for (int n = 2; QFile::exists(path); ++n) {
        path = dir + QLatin1Char('/') + QStringLiteral("%1 (%2).json").arg(baseName).arg(n);
    }
    return path;
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
    const QJsonValue name = o.value(QStringLiteral("name"));
    t.name = name.isString() ? name.toString() : QFileInfo(path).completeBaseName();
    for (const ColorSlot& slot : kColorSlots) {
        readColor(o, slot.key, t.*slot.member);
    }
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
    for (const ColorSlot& slot : kColorSlots) {
        o[QString::fromLatin1(slot.key)] = (theme.*slot.member).name(QColor::HexRgb);
    }
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

    // The user's themes, then the shipped examples; a user theme shadows an example of
    // the same name.
    QStringList seen;
    for (const QString& where : {themesDirectory(),
                                 setup::resourcePath(QStringLiteral("themes"))}) {
        if (where.isEmpty()) continue;
        QDir dir(where);
        const QStringList files =
            dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : files) {
            if (seen.contains(name)) continue;
            ThemeTokens t;
            // A broken file is skipped, not reported, so it cannot hide the working ones.
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
    if (!readTheme(sourcePath, t, error)) return {};
    // Keep the source's name, but never overwrite a theme already there.
    const QString target = newThemePath(QFileInfo(sourcePath).completeBaseName());
    return writeTheme(t, target, error) ? target : QString();
}

}  // namespace sonar::ui
