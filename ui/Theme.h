#pragma once

#include <QColor>
#include <QString>
#include <QVector>

class QApplication;
class QWidget;

namespace sonar::ui {

// Theming: one built-in dark theme, compiled in and never replaced, plus an optional
// theme layered over it — so a broken theme file can only change colours, and the
// built-in one is always a click away. A theme is a JSON file with thirteen colours
// (the rest are derived), a font family, and optional raw Qt style sheet appended
// after the built-in one.

struct ThemeTokens {
    QString name = QStringLiteral("Sonero Dark");

    QColor bg{0x0d, 0x0e, 0x14};        // window
    QColor sidebar{0x11, 0x12, 0x19};   // top bar, tray window
    QColor surface{0x19, 0x1b, 0x26};   // cards, channel strips
    QColor input{0x1f, 0x22, 0x30};     // fields, buttons, combo boxes
    QColor hover{0x26, 0x2a, 0x3a};     // what the pointer lightens
    QColor border{0x2a, 0x2e, 0x3f};
    QColor text{0xe7, 0xe9, 0xf2};
    QColor textDim{0x8b, 0x90, 0xa8};   // captions, hints, disabled-ish
    QColor accent{0x63, 0x66, 0xf1};    // indigo: selection, focus, the EQ curve
    QColor accentHi{0x8b, 0x8f, 0xf9};  // the accent, hovered
    QColor danger{0xff, 0x5c, 0x7a};    // mute, errors
    QColor warn{0xff, 0xb4, 0x54};      // solo, warnings
    QColor ok{0x4a, 0xde, 0x80};        // connected, healthy

    QString fontFamily =
        QStringLiteral("\"Inter\", \"Segoe UI\", \"Roboto\", \"Cantarell\", "
                       "\"Noto Sans\", sans-serif");

    // Appended after the built-in sheet, so it wins where it overlaps. Empty in
    // the built-in theme.
    QString extraQss;

    // Where this theme came from. Empty for the built-in one, which is also how
    // the UI knows it cannot be deleted or overwritten.
    QString filePath;

    [[nodiscard]] bool isBuiltIn() const { return filePath.isEmpty(); }
};

// The built-in theme: the one compiled into the binary.
[[nodiscard]] ThemeTokens builtInTheme();

// The theme in use; custom-painted widgets read their colours from it.
[[nodiscard]] const ThemeTokens& currentTheme();

// Apply a theme app-wide (Fusion, palette, the substituted built-in sheet, then its
// extra sheet); it becomes currentTheme().
void applyTheme(QApplication& app, const ThemeTokens& theme);

// The built-in theme, for callers that have no theme to apply yet.
void applyTheme(QApplication& app);

// Re-apply currentTheme() to every existing widget. Changing a theme while the
// app is running needs this; the custom-painted widgets repaint themselves.
void refreshTheme(QApplication& app);

// Linear sRGB mix — what the original palette was picked against.
[[nodiscard]] QColor mixColors(const QColor& a, const QColor& b, double t);

// Tag a widget with an application-sheet state colour ("ok", "warn", "bad", nullptr
// to clear) instead of a local setStyleSheet() the theme cannot reach.
void setState(QWidget* widget, const char* state);

// --- Theme files -------------------------------------------------------------
// In ~/.config/Sonero/themes as .json; Sonero only adds to it on an explicit import.

[[nodiscard]] QString themesDirectory();

// Every theme Sonero can offer, the built-in one first. Unreadable or malformed
// files are skipped rather than failing the list.
[[nodiscard]] QVector<ThemeTokens> availableThemes();

// Read one theme file. Returns false and leaves `out` untouched on anything it
// cannot parse; `error` then says what was wrong, for the UI to show.
bool readTheme(const QString& path, ThemeTokens& out, QString* error = nullptr);

// Write a theme out, for export and for "save the built-in one as a starting
// point". Creates the parent directory if needed.
bool writeTheme(const ThemeTokens& theme, const QString& path, QString* error = nullptr);

// A free "<baseName>.json" (or "<baseName> (n).json") in themesDirectory(), which
// is created if needed. Never the name of a theme already there.
[[nodiscard]] QString newThemePath(const QString& baseName);

// Copy a theme file into the themes directory, so it shows up in the list.
// Returns the path it landed at, or an empty string with `error` set.
[[nodiscard]] QString importTheme(const QString& sourcePath, QString* error = nullptr);

// The built-in sheet with `theme`'s colours substituted, as exported for reference.
[[nodiscard]] QString resolvedStyleSheet(const ThemeTokens& theme);

}  // namespace sonar::ui
