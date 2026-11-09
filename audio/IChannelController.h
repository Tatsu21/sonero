#pragma once

#include <string>

#include "audio/Channel.h"

namespace sonar::audio {

// Applies channel controls to the real audio path and reports real meters back.
class IChannelController {
public:
    virtual ~IChannelController() = default;

    // Set the linear volume (0..1) of a channel's virtual sink.
    virtual void setChannelVolume(ChannelId id, float volume) = 0;

    virtual void setChannelBalance(ChannelId id, float balance) = 0;

    // The channel's level in the stream mix a capture app records, independent of what
    // the user hears.
    virtual void setStreamLevel(ChannelId id, float level) = 0;
    [[nodiscard]] virtual float streamLevel(ChannelId id) const = 0;

    // Per-channel trim (linear, 1.0 = unity), multiplied with the fader.
    virtual void setChannelGain(ChannelId id, float gain) = 0;
    // Mute/unmute a channel's virtual sink.
    virtual void setChannelMute(ChannelId id, bool muted) = 0;

    // Real peak level captured from the channel's monitor since the previous
    // call (the accumulator is reset on read). Zero when no audio is flowing.
    virtual ChannelLevel channelLevel(ChannelId id) = 0;

    // Route a channel to a device by node.name ("" = the default output). Remembered and
    // re-applied when the device reconnects; falls back to the default meanwhile.
    virtual void setChannelOutput(ChannelId id, const std::string& deviceName) = 0;

    // The device node.name a channel is pinned to, or empty for the default.
    [[nodiscard]] virtual std::string channelOutput(ChannelId id) const = 0;
};

}  // namespace sonar::audio
