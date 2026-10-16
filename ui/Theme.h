#pragma once

#include <QColor>
#include <QString>
#include <QVector>

class QApplication;
class QWidget;

namespace sonar::ui {

// Theming. The look is one built-in dark theme plus whatever the user layers on
// top of it, and the built-in one is never replaced — it is compiled into the
// binary and is always the base a theme is applied over. That is deliberate: a
// theme file that is wrong, truncated or written for an older version can only
// change colours, never leave the app unusable, and "back to the built-in theme"
// is always one click away because there is nothing to restore from disk.
//
// A theme is a small JSON file: thirteen named colours, a font family, and an
// optional block of raw Qt style sheet appended after the built-in one for the
// things colours cannot express (a corner radius, a spacing, one specific
// widget). The colours are the supported surface; the style sheet is the escape
// hatch, and a theme that uses it can break its own layout.
//
// Everything the built-in sheet draws is derived from these thirteen. The shades
// in between — a hover a little lighter than its surface, a border a little
// lighter than that — are computed rather than listed, so a user supplying
// thirteen colours gets a coherent interface instead of thirty-six decisions.

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

// The theme in use right now. Custom-painted widgets (the equalizer curve, the
// VU meters, the channel strips' accents) ask this rather than carrying colours
// of their own, which is what lets a theme reach them too.
[[nodiscard]] const ThemeTokens& currentTheme();

// Apply a theme to the whole application: Fusion base, palette, and the built-in
// style sheet with this theme's colours substituted in, followed by its extra
// style sheet if it has one. Also becomes currentTheme().
void applyTheme(QApplication& app, const ThemeTokens& theme);

// The built-in theme, for callers that have no theme to apply yet.
void applyTheme(QApplication& app);

// Re-apply currentTheme() to every existing widget. Changing a theme while the
// app is running needs this; the custom-painted widgets repaint themselves.
void refreshTheme(QApplication& app);

// Tag a widget with one of the state colours the application sheet defines:
// "ok", "warn", "bad", or nullptr to clear. Use this instead of a local
// setStyleSheet("color:#...") — a widget that carries its own sheet is immune to
// the application sheet, and so immune to the theme.
//
// Repolishes the widget, because Qt only re-reads a style property then.
void setState(QWidget* widget, const char* state);

// --- Theme files -------------------------------------------------------------
//
// Themes live in ~/.config/Sonero/themes as .json files. The directory is the
// user's; Sonero only ever adds to it on an explicit import.

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

// Copy a theme file into the themes directory, so it shows up in the list.
// Returns the path it landed at, or an empty string with `error` set.
[[nodiscard]] QString importTheme(const QString& sourcePath, QString* error = nullptr);

// The built-in style sheet with `theme`'s colours substituted — what export
// writes alongside a theme so a user editing `extraQss` can see what they are
// overriding.
[[nodiscard]] QString resolvedStyleSheet(const ThemeTokens& theme);

}  // namespace sonar::ui
