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

// The volume faders, as a small window the tray opens.
//
// They cannot live in the tray menu itself: a desktop with a StatusNotifier host
// (GNOME with AppIndicator, KDE) receives the menu over DBusMenu, which carries
// only text, icons and checkmarks — a slider embedded as a QWidgetAction arrives
// there as an empty row. A real window has no such limit and behaves the same on
// every desktop.
//
// A decorated window, not a frameless pop-up. The first version was frameless and
// closed only by losing focus, which on Wayland it may never be told it did —
// leaving a window with no title bar, no close button and no Escape, pinned above
// everything by WindowStaysOnTopHint. That is indistinguishable from a hung
// desktop. Whatever else changes here, the user must always have a way out.
//
// Deliberately dumb, like ChannelStrip: it emits intent and reflects what it is
// told, so the one place that knows how a volume change reaches PipeWire stays
// MixerPage::applyChannelVolume().
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
