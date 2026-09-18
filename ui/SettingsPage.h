#pragma once

#include <QWidget>

class QVBoxLayout;

class QLabel;
class QProgressBar;
class QPushButton;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::update {
class Updater;
}

namespace sonar::ui {

class Notifier;

// Application preferences: the desktop-notification toggle plus the background /
// autostart options that drive the tray-based "run as a daemon" behavior.
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(Notifier* notifier, config::SettingsStore* settings = nullptr,
                          update::Updater* updater = nullptr, QWidget* parent = nullptr);

private:
    void buildUpdatesCard(QVBoxLayout* root);
    void buildSystemCard(QVBoxLayout* root);
    void buildAboutCard(QVBoxLayout* root);  // version, license, how it was built
    void refreshSystemChecks();  // re-run the checks and repaint the rows

    // Paint whatever the updater last told us: the version line, the button's
    // job right now, and the command for installs we do not touch ourselves.
    void showUpdateIdle();
    void showUpdateFound();

    Notifier* notifier_ = nullptr;
    config::SettingsStore* settings_ = nullptr;
    update::Updater* updater_ = nullptr;
    QVBoxLayout* systemBody_ = nullptr;  // rows are rebuilt in place

    // Update card, kept as members because the updater's signals repaint them.
    QLabel* updateStatus_ = nullptr;
    QPushButton* updateCheck_ = nullptr;
    QPushButton* updateAction_ = nullptr;  // download / restart / open the page
    QProgressBar* updateProgress_ = nullptr;
    QLabel* updateCommand_ = nullptr;  // the exact line to paste, when there is one
};

}  // namespace sonar::ui
