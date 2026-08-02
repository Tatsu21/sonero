#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QProgressBar;
class QSlider;
class QTimer;

namespace sonar::audio {
class IChannelController;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

// Dedicated microphone page: input gain, mute and a live level meter are wired to
// the real virtual mic source; noise suppression, noise gate and monitoring are
// prepared here as UI and will be connected to DSP in a later stage.
class MicrophonePage : public QWidget {
    Q_OBJECT

public:
    explicit MicrophonePage(audio::IChannelController* controller,
                            config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

private:
    void refresh();
    void saveMic();  // persist input gain + mute

    audio::IChannelController* controller_ = nullptr;
    config::SettingsStore* settings_ = nullptr;
    QTimer* timer_ = nullptr;
    QProgressBar* level_ = nullptr;
    QSlider* gain_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QCheckBox* mute_ = nullptr;
};

}  // namespace sonar::ui
