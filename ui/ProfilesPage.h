#pragma once

#include <QWidget>

#include "ui/Theme.h"

class QComboBox;
class QLabel;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

// Profiles: the page for things that describe how Sonero looks and behaves
// rather than what it does to audio. Today that is the theme.
//
// Picking a theme applies it immediately and remembers it. The built-in theme is
// always the first entry and cannot be removed, so there is always a way back
// from a theme that turned out to be unreadable — it is compiled into the
// binary, not loaded from disk.
class ProfilesPage : public QWidget {
    Q_OBJECT

public:
    explicit ProfilesPage(config::SettingsStore* settings = nullptr,
                          QWidget* parent = nullptr);

    // Reads the remembered theme and applies it. Called once at startup, before
    // the window is shown, so a custom theme is never preceded by a flash of the
    // built-in one.
    static void applySavedTheme(config::SettingsStore* settings);

private:
    void reloadThemes(const QString& selectPath = QString());
    void applySelected();
    void onImport();
    void onExportTheme();
    void onExportStyleSheet();
    void onOpenFolder();
    void setStatus(const QString& text, bool bad = false);

    [[nodiscard]] ThemeTokens selectedTheme() const;

    config::SettingsStore* settings_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* detail_ = nullptr;
    QVector<ThemeTokens> themes_;
};

}  // namespace sonar::ui
