#pragma once

#include <string>

#include "audio/Channel.h"

namespace sonar::audio {

// Applies channel controls to the real audio path (the virtual sink nodes) and
// reports real metering back. Implemented by the PipeWire backend; kept separate
// from IAppRouter so the UI depends only on the capabilities it uses.
class IChannelController {
public:
    virtual ~IChannelController() = default;

    // Set the linear volume (0..1) of a channel's virtual sink.
    virtual void setChannelVolume(ChannelId id, float volume) = 0;

    virtual void setChannelBalance(ChannelId id, float balance) = 0;

    // Linear attenuation this channel currently applies on top of the fader:
    // equalizer-boost compensation times the fixed filter reserve. Metering taps
    // the output, i.e. after this, so anything judging the *source* level has to
    // divide it back out.
    [[nodiscard]] virtual float channelHeadroom(ChannelId id) const = 0;

    // How loud this channel is in the stream mix — the separate mix a capture
    // application records. Independent of what the user hears, which is the point:
    // you can keep game audio loud in your headphones and quiet on stream.
    virtual void setStreamLevel(ChannelId id, float level) = 0;
    [[nodiscard]] virtual float streamLevel(ChannelId id) const = 0;

    // Per-channel trim (linear, 1.0 = unity), independent of the volume fader.
    // Use it to tame a source that is far louder or quieter than the others; it
    // multiplies with the fader rather than replacing it.
    virtual void setChannelGain(ChannelId id, float gain) = 0;
    // Mute/unmute a channel's virtual sink.
    virtual void setChannelMute(ChannelId id, bool muted) = 0;

    // Real peak level captured from the channel's monitor since the previous
    // call (the accumulator is reset on read). Zero when no audio is flowing.
    virtual ChannelLevel channelLevel(ChannelId id) = 0;

    // Route a channel's output to a specific device by its node.name (as in
    // AudioDevice::name); an empty name follows the system default output. The
    // choice is remembered and re-applied when the device (re)connects, and falls
    // back to the default while the device is gone.
    virtual void setChannelOutput(ChannelId id, const std::string& deviceName) = 0;

    // The device node.name a channel is pinned to, or empty for the default.
    [[nodiscard]] virtual std::string channelOutput(ChannelId id) const = 0;
};

}  // namespace sonar::audio
