#include "audio/Mixer.h"

#include <algorithm>

#include "core/Log.h"

namespace sonar::audio {

namespace {
float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }
float clampBalance(float v) noexcept { return std::clamp(v, -1.0f, 1.0f); }
}  // namespace

Mixer::Mixer() {
    // Sensible, non-uniform defaults so a fresh mixer looks alive.
    
    strips_[channelIndex(ChannelId::System)].state.volume = 0.80f;
    strips_[channelIndex(ChannelId::Game)].state.volume = 0.80f;
    strips_[channelIndex(ChannelId::Chat)].state.volume = 0.70f;
    strips_[channelIndex(ChannelId::Media)].state.volume = 0.60f;
    strips_[channelIndex(ChannelId::Browser)].state.volume = 0.50f;
    strips_[channelIndex(ChannelId::Microphone)].state.volume = 0.70f;
    strips_[channelIndex(ChannelId::Aux)].state.volume = 0.40f;
}

std::vector<ChannelId> Mixer::channels() const {
    return std::vector<ChannelId>(kAllChannels.begin(), kAllChannels.end());
}

ChannelState Mixer::state(ChannelId id) const {
    return strips_[channelIndex(id)].state;
}

void Mixer::setVolume(ChannelId id, float volume) {
    strips_[channelIndex(id)].state.volume = clamp01(volume);
    log::debug("Mixer: {} volume -> {:.0f}%", channelName(id),
               strips_[channelIndex(id)].state.volume * 100.0f);
}

void Mixer::setMuted(ChannelId id, bool muted) {
    strips_[channelIndex(id)].state.muted = muted;
    log::debug("Mixer: {} {}", channelName(id), muted ? "muted" : "unmuted");
}

void Mixer::setSolo(ChannelId id, bool solo) {
    strips_[channelIndex(id)].state.solo = solo;
    log::debug("Mixer: {} solo {}", channelName(id), solo ? "on" : "off");
}

void Mixer::setBalance(ChannelId id, float balance) {
    strips_[channelIndex(id)].state.balance = clampBalance(balance);
    log::debug("Mixer: {} balance -> {:.0f}", channelName(id), strips_[channelIndex(id)].state.balance * 100.0f);
}

bool Mixer::anySolo() const noexcept {
    return std::any_of(strips_.begin(), strips_.end(),
                       [](const Strip& s) { return s.state.solo; });
}

bool Mixer::isAudible(ChannelId id) const {
    const ChannelState& s = strips_[channelIndex(id)].state;
    if (s.muted) {
        return false;
    }
    return !anySolo() || s.solo;
}

void Mixer::setLevel(ChannelId id, ChannelLevel value) {
    value.peakLeft = clamp01(value.peakLeft);
    value.peakRight = clamp01(value.peakRight);
    strips_[channelIndex(id)].level = value;
}

ChannelLevel Mixer::level(ChannelId id) const {
    return strips_[channelIndex(id)].level;
}

}  // namespace sonar::audio
