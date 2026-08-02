#pragma once

#include <QWidget>

class QVBoxLayout;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class Notifier;

// Application preferences: the desktop-notification toggle plus the background /
// autostart options that drive the tray-based "run as a daemon" behavior.
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(Notifier* notifier, config::SettingsStore* settings = nullptr,
                          QWidget* parent = nullptr);

private:
    void buildSystemCard(QVBoxLayout* root);
    void refreshSystemChecks();  // re-run the checks and repaint the rows

    Notifier* notifier_ = nullptr;
    config::SettingsStore* settings_ = nullptr;
    QVBoxLayout* systemBody_ = nullptr;  // rows are rebuilt in place
};

}  // namespace sonar::ui
