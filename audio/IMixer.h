#pragma once

#include <vector>

#include "audio/Channel.h"

namespace sonar::audio {

// Abstraction over the channel mixer. The UI drives it (user actions) and reads
// metering back from it; a future AudioEngine will feed real levels into it.
// Keeping this Qt-free makes the mixing logic unit-testable in isolation.
class IMixer {
public:
    virtual ~IMixer() = default;

    [[nodiscard]] virtual std::vector<ChannelId> channels() const = 0;

    [[nodiscard]] virtual ChannelState state(ChannelId id) const = 0;
    virtual void setVolume(ChannelId id, float volume) = 0;    // clamped to 0..1
    virtual void setMuted(ChannelId id, bool muted) = 0;
    virtual void setSolo(ChannelId id, bool solo) = 0;
    virtual void setBalance(ChannelId id, float balance) = 0;  // clamped to -1..1

    // Whether the channel would currently be heard, accounting for mute and for
    // any active solo elsewhere.
    [[nodiscard]] virtual bool isAudible(ChannelId id) const = 0;

    // Metering: written by the audio path (or the placeholder simulator for now),
    // read by the UI.
    virtual void setLevel(ChannelId id, ChannelLevel level) = 0;
    [[nodiscard]] virtual ChannelLevel level(ChannelId id) const = 0;
};

}  // namespace sonar::audio
