#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "audio/Channel.h"

namespace sonar::audio {

// A running application producing audio, as seen in the PipeWire graph.
struct AppStream {
    uint32_t id = 0;                    // PipeWire node id
    std::string name;                   // display name (application.name / media.name)
    std::optional<ChannelId> channel = ChannelId::System;   // channel it has been routed to, if any
};

// Routes application audio streams onto the virtual channel sinks. Implemented
// by the PipeWire backend; kept separate from IAudioBackend so the UI depends
// only on the routing capability it needs.
class IAppRouter {
public:
    virtual ~IAppRouter() = default;

    // Snapshot of the current application streams (thread-safe).
    [[nodiscard]] virtual std::vector<AppStream> applications() const = 0;

    // Route an application's stream onto a channel's virtual sink.
    // Returns false if the target sink is not ready or routing is unavailable.
    virtual bool assign(uint32_t appId, ChannelId channel) = 0;

    // Monotonic counter bumped whenever the application list changes, so the UI
    // can cheaply detect when it needs to rebuild.
    [[nodiscard]] virtual std::uint64_t revision() const = 0;
};

}  // namespace sonar::audio
