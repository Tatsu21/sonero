#pragma once

#include <cstdint>
#include <random>
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
class IAudioDevices;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class ChannelStrip;
class FlowLayout;

// The Mixer page: one ChannelStrip per channel, wired to the mixer model, plus
// an "Applications" panel (from the router) that lets each running app be moved
// onto a channel. A refresh timer drives the VU meters and rebuilds the app list
// when the router reports a change.
//
// Until the audio engine produces real peaks, an optional (default-on)
// placeholder simulator synthesizes levels from channel state.
class MixerPage : public QWidget {
    Q_OBJECT

public:
    MixerPage(sonar::audio::IMixer& mixer, sonar::audio::IAppRouter* router,
              sonar::audio::IChannelController* controller,
              sonar::audio::IAudioDevices* devices = nullptr,
              sonar::config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

private:
    void refresh();
    void injectSimulatedLevels();
    void rebuildApplications();
    void pushChannelBalances();
    void pushChannelVolumes();  // apply every channel's volume to the real sinks
    void pushMuteStates();      // apply mute+solo to the real channel sinks
    void syncOutputDevices();   // refresh each strip's output selector
    void updateAutoGain(ChannelStrip* strip, float peak);  // one AGC step
    void restoreMixerState();   // load persisted channel state into the model
    void saveMixerState();      // snapshot the model into the settings store

    sonar::audio::IMixer& mixer_;
    sonar::audio::IAppRouter* router_ = nullptr;
    sonar::audio::IChannelController* controller_ = nullptr;
    sonar::audio::IAudioDevices* devices_ = nullptr;
    sonar::config::SettingsStore* settings_ = nullptr;

    std::vector<ChannelStrip*> strips_;
    std::unordered_map<int, ChannelStrip*> stripByChannel_;
    // Per-channel trim in dB, persisted alongside the rest of the channel state.
    std::unordered_map<int, float> channelGainDb_;
    // Auto-gain: one control loop per channel, fed by the meters in refresh().
    std::unordered_map<int, bool> autoGainOn_;
    std::unordered_map<int, dsp::AutoGain> autoGain_;
    FlowLayout* appsLayout_ = nullptr;
    QTimer* timer_ = nullptr;
    bool simulate_ = true;
    std::uint64_t appsRevision_ = ~0ULL;     // force initial rebuild
    std::uint64_t devicesRevision_ = ~0ULL;  // force initial output-selector fill

    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> noise_{0.0f, 1.0f};
};

}  // namespace sonar::ui
