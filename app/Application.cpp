#include "app/Application.h"

#include "app/AutoStart.h"

#include "app/SystemSetup.h"

#include "core/Version.h"
#include "audio/IAppRouter.h"
#include "audio/IAudioDevices.h"
#include "audio/IChannelController.h"
#include "audio/IDeviceFormats.h"
#include "audio/IEqualizerController.h"
#include "audio/IMixer.h"
#include "core/Log.h"
#include "ui/MainWindow.h"

namespace sonar {

Application::Application(std::unique_ptr<audio::IAudioBackend> backend,
                         std::unique_ptr<audio::IMixer> mixer)
    : backend_(std::move(backend)),
      mixer_(std::move(mixer)),
      router_(dynamic_cast<audio::IAppRouter*>(backend_.get())),
      controller_(dynamic_cast<audio::IChannelController*>(backend_.get())),
      eqController_(dynamic_cast<audio::IEqualizerController*>(backend_.get())),
      deviceFormats_(dynamic_cast<audio::IDeviceFormats*>(backend_.get())),
      audioDevices_(dynamic_cast<audio::IAudioDevices*>(backend_.get())) {}

// Defined here (not defaulted in the header) so unique_ptr<MainWindow> can see
// the complete type when generating the destructor.
Application::~Application() = default;

void Application::start(bool background) {
    log::info("Sonero {} starting{}", versionString(), background ? " (background)" : "");

    // Portable (AppImage) runs register themselves with the desktop on first
    // start: menu entry + icons under ~/.local/share. Purely user-level and
    // idempotent — anything needing root is offered on the Settings page instead.
    if (setup::runningFromAppImage()) {
        // Read the old bundle's path first — installDesktopIntegration() is about
        // to overwrite the entry that holds it — but delete it only once the entry
        // names this bundle instead. The other order leaves the app menu pointing
        // at a file that no longer exists.
        const QString previous = setup::previousBundlePath();
        if (setup::installDesktopIntegration()) {
            log::info("Desktop integration installed (menu entry + icons)");
            if (const QString removed = setup::removeSupersededBundle(previous);
                !removed.isEmpty()) {
                log::info("Removed the superseded AppImage {}", removed.toStdString());
            }
        } else {
            log::warn("Could not install desktop integration");
        }
    }

    // The autostart entry holds a path, and a newer AppImage lives at a different
    // one. Without this, "start at login" keeps launching the bundle it was
    // switched on with — or silently stops working once that file is deleted.
    if (autostart::refreshExecPath()) {
        log::info("Autostart entry re-pointed at {}",
                  autostart::executablePath().toStdString());
    }

    if (backend_->initialize()) {
        const auto info = backend_->serverInfo();
        log::info("Audio backend '{}' available (server version {})",
                  backend_->name(), info.version);
    } else {
        log::warn("Audio backend '{}' unavailable — UI runs in offline mode",
                  backend_->name());
    }

    window_ = std::make_unique<ui::MainWindow>(*backend_, *mixer_, router_, controller_,
                                               eqController_, deviceFormats_, audioDevices_);
    // --background (used by the autostart entry) keeps the audio graph running with
    // no window; the user reopens it from the tray or by launching the app again.
    if (!background) {
        window_->show();
    }
}

QWidget* Application::mainWindow() const { return window_.get(); }

}  // namespace sonar
