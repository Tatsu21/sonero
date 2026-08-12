#pragma once

#include <cstdint>

#include <QList>
#include <QPair>
#include <QStringList>
#include <QWidget>

#include "audio/Channel.h"

class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QPushButton;
class QCheckBox;
class QSlider;
class QBoxLayout;
class QVBoxLayout;

namespace sonar::ui {

class VuMeter;

// One vertical channel column: name, VU meter, volume fader, balance, mute and
// solo. The strip is deliberately "dumb": it emits intent via signals and lets
// the owning page apply them to the mixer, then reflects state back through
// setState()/setLevel().
class ChannelStrip : public QWidget {
    Q_OBJECT

public:
    ChannelStrip(sonar::audio::ChannelId id, const QString& name,
                 QWidget* parent = nullptr);

    [[nodiscard]] sonar::audio::ChannelId channelId() const noexcept { return id_; }

    void setState(const sonar::audio::ChannelState& state);
    void setLevel(float left, float right);
    // Accent colour and glyph that identify the channel at a glance.
    [[nodiscard]] static QString accentFor(sonar::audio::ChannelId id);
    [[nodiscard]] static QString glyphFor(sonar::audio::ChannelId id);

    // Application chips live inside the strip, so a channel shows what it is
    // actually carrying. MixerPage owns the chips and hands them over here.
    void clearApps();
    void addApp(QWidget* chip);

    // Stream mode reveals a second fader: how loud this channel is in the mix a
    // capture application records, independent of what the user hears.
    void setStreamMode(bool on);
    void setStreamLevel(float level);  // 0..1, without emitting

signals:
    void volumeChanged(sonar::audio::ChannelId id, float volume);   // 0..1
    void muteToggled(sonar::audio::ChannelId id, bool muted);
    void soloToggled(sonar::audio::ChannelId id, bool solo);
    void balanceChanged(sonar::audio::ChannelId id, float balance);  // -1..1
    void streamLevelChanged(sonar::audio::ChannelId id, float level);  // 0..1
    void appDropped(sonar::audio::ChannelId id, std::uint32_t appId);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setDropActive(bool active);
    void updateVolumeLabel(int percent);

    sonar::audio::ChannelId id_;
    VuMeter* meter_ = nullptr;
    QSlider* volume_ = nullptr;
    QSlider* balance_ = nullptr;
    QPushButton* mute_ = nullptr;
    QPushButton* solo_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QLabel* appsEmpty_ = nullptr;
    QVBoxLayout* appsBody_ = nullptr;
    QWidget* streamRow_ = nullptr;
    QSlider* stream_ = nullptr;
    QLabel* streamValue_ = nullptr;
};

}  // namespace sonar::ui
