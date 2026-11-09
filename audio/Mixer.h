#pragma once

#include <array>

#include "audio/Channel.h"
#include "audio/IMixer.h"

namespace sonar::audio {

// In-memory mixer model for the fixed set of channels; every change is clamped to a
// valid range. UI thread only.
class Mixer final : public IMixer {
public:
    Mixer();

    [[nodiscard]] std::vector<ChannelId> channels() const override;

    [[nodiscard]] ChannelState state(ChannelId id) const override;
    void setVolume(ChannelId id, float volume) override;
    void setMuted(ChannelId id, bool muted) override;
    void setSolo(ChannelId id, bool solo) override;
    void setBalance(ChannelId id, float balance) override;

    [[nodiscard]] bool isAudible(ChannelId id) const override;

    void setLevel(ChannelId id, ChannelLevel level) override;
    [[nodiscard]] ChannelLevel level(ChannelId id) const override;

private:
    struct Strip {
        ChannelState state;
        ChannelLevel level;
    };

    [[nodiscard]] bool anySolo() const noexcept;

    std::array<Strip, kAllChannels.size()> strips_{};
};

}  // namespace sonar::audio
