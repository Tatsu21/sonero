#pragma once

#include <memory>

#include <QList>
#include <QMainWindow>

#include "ui/DevicesPage.h"  // BatteryLevel, reported to the tray menu

namespace sonar::audio {
class IAudioBackend;
class IMixer;
class IAppRouter;
class IChannelController;
class IEqualizerController;
class IDeviceFormats;
class IAudioDevices;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {
class Notifier;
class MixerPage;
class TrayMixer;
}

class QAction;
class QMenu;

class QButtonGroup;
class QListWidget;
class QStackedWidget;
class QSystemTrayIcon;
class QCloseEvent;

namespace sonar::ui {

// Top-level window: a navigation sidebar on the left and a stack of pages on the
// right. For Stage 1 only the Dashboard is functional (it reports the audio
// backend status); the other pages are placeholders for later stages.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const audio::IAudioBackend& backend, audio::IMixer& mixer,
               audio::IAppRouter* router, audio::IChannelController* controller,
               audio::IEqualizerController* eqController,
               audio::IDeviceFormats* deviceFormats = nullptr,
               audio::IAudioDevices* audioDevices = nullptr, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Closing the window hides it to the tray (keeping PipeWire sinks alive) when
    // "run in background" is enabled; a full quit goes through the tray's Quit action.
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void buildTrayIcon();
    void showTrayMixer();  // the volume faders, as a window the tray opens
    // Rewrites the battery entries at the top of the tray menu.
    void showBatteries(const QList<BatteryLevel>& levels);
    void showBackgroundHintOnce();  // one-time "still running" hint on first hide
    [[nodiscard]] bool runInBackgroundEnabled() const;
    void quitApplication();
    QWidget* createDashboardPage();
    QWidget* createPlaceholderPage(const QString& title, const QString& subtitle);

    const audio::IAudioBackend& backend_;
    audio::IMixer& mixer_;
    audio::IAppRouter* router_ = nullptr;
    audio::IChannelController* controller_ = nullptr;
    audio::IEqualizerController* eqController_ = nullptr;
    audio::IDeviceFormats* deviceFormats_ = nullptr;
    audio::IAudioDevices* audioDevices_ = nullptr;
    config::SettingsStore* settings_ = nullptr;  // owned via QObject parenting
    Notifier* notifier_ = nullptr;                // owned via QObject parenting
    MixerPage* mixerPage_ = nullptr;  // owns the per-channel gain / auto state
    QButtonGroup* navGroup_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QSystemTrayIcon* trayIcon_ = nullptr;         // owned via QObject parenting
    DevicesPage* devicesPage_ = nullptr;          // seeds the tray's battery entries
    // Its own top-level window, not a child: a Qt::Tool child is hidden along
    // with its parent, and the tray must stay usable while the window is hidden.
    std::unique_ptr<TrayMixer> trayMixer_;
    QMenu* trayMenu_ = nullptr;                   // null when no tray is available
    QList<QAction*> batteryActions_;              // rebuilt as levels change
    bool forceQuit_ = false;                       // set by the tray's Quit action
};

}  // namespace sonar::ui
