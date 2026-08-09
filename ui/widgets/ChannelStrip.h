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
    // Show a gain the app computed (auto mode) without emitting gainChanged.
    void showGainDb(float gainDb);
    void setAutoGain(bool on);
    void setLevel(float left, float right);
    void setAssignedApps(const QStringList& appNames);

    // Populate the output selector. Each entry is {display label, device
    // node.name}; the first is conventionally {"Default", ""}. `currentNodeName`
    // is the entry to pre-select ("" = Default).
    void setOutputDevices(const QList<QPair<QString, QString>>& devices,
                          const QString& currentNodeName);

signals:
    void volumeChanged(sonar::audio::ChannelId id, float volume);   // 0..1
    void muteToggled(sonar::audio::ChannelId id, bool muted);
    void soloToggled(sonar::audio::ChannelId id, bool solo);
    void balanceChanged(sonar::audio::ChannelId id, float balance);  // -1..1
    // Per-channel trim in dB (-20..+6); 0 dB is unity.
    void gainChanged(sonar::audio::ChannelId id, float gainDb);
    void autoGainToggled(sonar::audio::ChannelId id, bool on);
    void appDropped(sonar::audio::ChannelId id, std::uint32_t appId);
    void outputChanged(sonar::audio::ChannelId id, const QString& deviceNodeName);

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
    QSlider* gain_ = nullptr;
    QCheckBox* autoGain_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QPushButton* mute_ = nullptr;
    QPushButton* solo_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QLabel* appsLabel_ = nullptr;
    QComboBox* output_ = nullptr;
};

}  // namespace sonar::ui
