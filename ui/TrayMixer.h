#pragma once

#include <unordered_map>

#include <QWidget>

#include "audio/Channel.h"

class QLabel;
class QSlider;

namespace sonar::audio {
class IMixer;
}

namespace sonar::ui {

// The volume faders as a small window the tray opens — a tray menu travels over
// DBusMenu, where an embedded slider arrives as an empty row. A decorated window,
// not a frameless pop-up: on Wayland a pop-up closed only by focus loss could get
// stuck on top with no way out. It only emits intent and reflects state;
// MixerPage::applyChannelVolume() applies it.
class TrayMixer : public QWidget {
    Q_OBJECT

public:
    explicit TrayMixer(audio::IMixer& mixer, QWidget* parent = nullptr);

    // Show (or re-raise) near the pointer. Best effort on Wayland, which does not
    // let a client place its own windows.
    void popUp();

    // Reflect a change made elsewhere (the Mixer page) without emitting.
    void showVolume(audio::ChannelId id, float volume);

signals:
    void volumeChanged(audio::ChannelId id, float volume);  // 0..1

protected:
    // Escape closes, on top of the title bar's close button: with the pointer
    // still over a slider there is no button under it to click.
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Row {
        QSlider* slider = nullptr;
        QLabel* value = nullptr;
    };

    audio::IMixer& mixer_;
    std::unordered_map<int, Row> rows_;
};

}  // namespace sonar::ui
