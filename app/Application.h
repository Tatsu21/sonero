#pragma once

#include <memory>

#include "audio/IAudioBackend.h"

class QWidget;

namespace sonar::audio {
class IMixer;
class IAppRouter;
class IChannelController;
class IEqualizerController;
class IDeviceFormats;
class IAudioDevices;
}

namespace sonar::ui {
class MainWindow;
}

namespace sonar {

// Composition root of the program. It receives its dependencies (the audio
// backend and the mixer) via the constructor and owns the top-level UI. Keeping
// wiring here — rather than in main() — makes the object graph explicit and
// testable.
class Application {
public:
    Application(std::unique_ptr<audio::IAudioBackend> backend,
                std::unique_ptr<audio::IMixer> mixer);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Initializes the backend and shows the main window. With `background` the
    // window is created but left hidden (autostart / daemon mode).
    void start(bool background = false);

    // The top-level window (null before start()). Non-owning.
    [[nodiscard]] QWidget* mainWindow() const;

private:
    // Declared before the window so they outlive every UI element that uses them.
    std::unique_ptr<audio::IAudioBackend> backend_;
    std::unique_ptr<audio::IMixer> mixer_;
    // Non-owning; point at backend_ when it also provides these (may be null).
    audio::IAppRouter* router_ = nullptr;
    audio::IChannelController* controller_ = nullptr;
    audio::IEqualizerController* eqController_ = nullptr;
    audio::IDeviceFormats* deviceFormats_ = nullptr;
    audio::IAudioDevices* audioDevices_ = nullptr;
    std::unique_ptr<ui::MainWindow> window_;
};

}  // namespace sonar
