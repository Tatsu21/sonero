#pragma once

#include <string>
#include <string_view>

namespace sonar::audio {

// Lifecycle state of an audio backend connection.
enum class BackendState {
    Uninitialized,  // initialize() has not run (or shutdown() reset it)
    Available,      // connected to a working audio server
    Unavailable     // initialize() ran but no server could be reached
};

// Descriptive information about the connected audio server.
struct AudioServerInfo {
    std::string version;
    std::string name;
    std::string userName;
    std::string hostName;
};

// Abstraction over the system audio server. Depending on this interface (rather
// than on PipeWireManager directly) keeps the UI and the future AudioEngine
// decoupled from the backend and makes both trivially testable with fakes.
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // Connect to the audio server and probe availability.
    // Returns true when the backend ends up in the Available state.
    virtual bool initialize() = 0;

    // Disconnect and return to the Uninitialized state. Safe to call twice.
    virtual void shutdown() = 0;

    [[nodiscard]] virtual BackendState state() const noexcept = 0;
    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual AudioServerInfo serverInfo() const = 0;
};

}  // namespace sonar::audio
