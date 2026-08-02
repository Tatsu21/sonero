#pragma once

#include <array>
#include <string_view>

namespace sonar::audio {

// The fixed set of mixer channels. These map 1:1 onto the virtual PipeWire
// devices that a later stage will create (Game, Chat, Media, ...).
enum class ChannelId { System, Game, Chat, Media, Browser, Microphone, Aux };

inline constexpr std::array<ChannelId, 7> kAllChannels = {
    ChannelId::System, ChannelId::Game,  ChannelId::Chat,       ChannelId::Media,
    ChannelId::Browser, ChannelId::Microphone, ChannelId::Aux};

constexpr std::string_view channelName(ChannelId id) noexcept {
    switch (id) {
        case ChannelId::System:      return "System";
        case ChannelId::Game:       return "Game";
        case ChannelId::Chat:       return "Chat";
        case ChannelId::Media:      return "Media";
        case ChannelId::Browser:    return "Browser";
        case ChannelId::Microphone: return "Microphone";
        case ChannelId::Aux:        return "Aux";
    }
    return "Unknown";
}

constexpr std::size_t channelIndex(ChannelId id) noexcept {
    return static_cast<std::size_t>(id);
}

// User-controllable state of a single channel.
struct ChannelState {
    float volume = 0.75f;   // linear gain, 0.0 .. 1.0
    bool muted = false;
    bool solo = false;
    float balance = 0.0f;   // -1.0 (full left) .. +1.0 (full right)
};

// Instantaneous metering data for a channel (linear peak, 0.0 .. 1.0).
struct ChannelLevel {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
};

}  // namespace sonar::audio
