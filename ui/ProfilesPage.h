#pragma once

#include <QWidget>

#include "ui/Theme.h"

class QComboBox;
class QLabel;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

// Profiles: how Sonero looks — today, the theme. Applied immediately and remembered;
// the built-in theme (compiled in) is always first, so there is always a way back.
class ProfilesPage : public QWidget {
    Q_OBJECT

public:
    explicit ProfilesPage(config::SettingsStore* settings = nullptr,
                          QWidget* parent = nullptr);

    // Apply the remembered theme, at startup, before the window shows.
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
