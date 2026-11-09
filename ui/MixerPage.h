#pragma once

#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include <QWidget>

#include "audio/Channel.h"
#include "dsp/AutoGain.h"

class QTimer;

namespace sonar::audio {
class IMixer;
class IAppRouter;
class IChannelController;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class ChannelStrip;
class FlowLayout;

// The Mixer page: one ChannelStrip per channel plus a pool of unrouted apps to drag
// onto them. A timer drives the meters and rebuilds the apps on router changes.
class MixerPage : public QWidget {
    Q_OBJECT

public:
    MixerPage(sonar::audio::IMixer& mixer, sonar::audio::IAppRouter* router,
              sonar::audio::IChannelController* controller,
              sonar::config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

signals:
    void channelGainDbChanged(audio::ChannelId id, float gainDb);

    // A channel's own fader moved, wherever it was moved from. Lets a second view
    // of the same channel (the tray mixer) follow without polling.
    void channelVolumeChanged(audio::ChannelId id, float volume);

public:
    // The single path that changes a channel's volume: model, PipeWire (with the master
    // multiplier), persistence and this page's strip.
    void applyChannelVolume(audio::ChannelId id, float volume);

    // Called by the Channels page, which owns the gain / auto / output controls
    // while the metering loop that drives auto-gain stays here.
    void applyChannelGainDb(audio::ChannelId id, float gainDb);
    void applyChannelAutoGain(audio::ChannelId id, bool on);
    void applyChannelOutput(audio::ChannelId id, const QString& deviceNodeName);
    [[nodiscard]] float channelGainDb(audio::ChannelId id) const;
    // The device this channel plays to, so the Channels page can show it. The
    // backend is the authority — it is what the router actually applied.
    [[nodiscard]] QString channelOutput(audio::ChannelId id) const;
    [[nodiscard]] bool channelAutoGain(audio::ChannelId id) const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateRefreshRate();  // fast while shown or while auto-gain runs
    void refresh();
    void rebuildApplications();
    void pushChannelBalances();
    void pushChannelVolumes();  // apply every channel's volume to the real sinks
    void pushMuteStates();      // apply mute+solo to the real channel sinks
    // Set a channel's trim on the real sink, in dB.
    void pushChannelGain(audio::ChannelId id, float gainDb);
    void updateAutoGain(ChannelStrip* strip, float peak);  // one AGC step
    void restoreAppRouting();   // re-apply saved app->channel assignments
    void saveAppRouting(const QString& appName, audio::ChannelId channel);
    void restoreMixerState();   // load persisted channel state into the model
    void saveMixerState();      // snapshot the model into the settings store

    sonar::audio::IMixer& mixer_;
    sonar::audio::IAppRouter* router_ = nullptr;
    sonar::audio::IChannelController* controller_ = nullptr;
    sonar::config::SettingsStore* settings_ = nullptr;

    std::vector<ChannelStrip*> strips_;
    std::unordered_map<int, ChannelStrip*> stripByChannel_;
    // Per-channel gain in dB, persisted alongside the rest of the channel state.
    std::unordered_map<int, float> channelGainDb_;
    // Auto-gain: one control loop per channel, fed by the meters in refresh().
    std::unordered_map<int, bool> autoGainOn_;
    std::unordered_map<int, dsp::AutoGain> autoGain_;
    // Second mix: how loud each channel is for a capture application.
    std::unordered_map<int, float> streamLevel_;

    // App routing remembered by application name (node ids change every run). Placed
    // ids are tracked so a later manual move is not undone.
    std::unordered_map<QString, int> savedAppChannel_;
    std::set<std::uint32_t> routedAppIds_;
    FlowLayout* appsLayout_ = nullptr;
    // The unrouted pool, hidden entirely when every stream already has a channel
    // — otherwise it costs the strips the height their app chips need.
    QWidget* appsSection_ = nullptr;
    QTimer* timer_ = nullptr;
    std::uint64_t appsRevision_ = ~0ULL;  // force initial rebuild
};

}  // namespace sonar::ui
