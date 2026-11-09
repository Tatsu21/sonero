#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "audio/IAudioDevices.h"  // DeviceType
#include "audio/MicProcessor.h"   // MicLevels
#include "dsp/MicChain.h"

namespace sonar::audio {

// A real input device the microphone can capture from (never one of our own
// sonero_* sources: capturing those would feed Sonero back into itself).
struct MicInputDevice {
    std::string name;         // node.name (stable identifier)
    std::string description;  // human-friendly label
    DeviceType type = DeviceType::Other;
};

// The microphone's processing chain as the UI sees it: switchable stages, each
// reporting how hard it works. Implemented by the backend that owns the node.
class IMicrophoneController {
public:
    virtual ~IMicrophoneController() = default;

    virtual void setMicSettings(const dsp::MicSettings& settings) = 0;
    [[nodiscard]] virtual dsp::MicSettings micSettings() const = 0;

    // Levels and per-stage gain reduction. Reading clears the peak holds, so
    // exactly one place in the UI should poll it.
    [[nodiscard]] virtual MicLevels micLevels() = 0;

    virtual void setMicNoiseSuppression(bool on) = 0;
    [[nodiscard]] virtual bool micNoiseSuppression() const = 0;
    // False when the build has no model at all — the UI then says so instead of
    // offering a switch that cannot do anything.
    [[nodiscard]] virtual bool micNoiseSuppressionAvailable() const = 0;
    [[nodiscard]] virtual std::string_view micNoiseSuppressionBackend() const = 0;

    // Currently connected input devices, sorted by description.
    [[nodiscard]] virtual std::vector<MicInputDevice> micInputDevices() const = 0;
    // Bumped whenever the list, the system default or the active device changes.
    // The UI polls this and only rebuilds when it moves.
    [[nodiscard]] virtual std::uint64_t micInputRevision() const = 0;

    // The chosen input ("" = system default). Survives unplugging: the default stands
    // in until the device returns.
    virtual bool setMicCaptureDevice(const std::string& nodeName) = 0;
    [[nodiscard]] virtual std::string micCaptureDevice() const = 0;
    // What the chain is actually attached to right now; empty means nothing.
    [[nodiscard]] virtual std::string micActiveCaptureDevice() const = 0;
    // The system's default input, as last announced by the session manager.
    [[nodiscard]] virtual std::string micDefaultInput() const = 0;

    // Keep the chain running with no application recording, for as long as
    // somebody is looking at its meters. Off, the device is left to suspend.
    virtual void setMicLive(bool live) = 0;

    // "Hear myself": the processed voice on the default output. `level` is 0..1.
    virtual void setMicMonitor(bool on, float level) = 0;

    // Echo cancellation: what plays on `speakers` (node.name, "" = default) is removed
    // from the microphone. Inactive while off, with no mic or output, or when PipeWire's
    // echo-cancel module could not be loaded.
    virtual void setMicEchoCancellation(bool on, const std::string& speakers) = 0;
    [[nodiscard]] virtual bool micEchoCancellationActive() const = 0;
    // The output it is listening to right now; empty while it is not running.
    [[nodiscard]] virtual std::string micEchoReference() const = 0;
    // The outputs the speakers can be on, and the system default among them.
    [[nodiscard]] virtual std::vector<AudioDevice> micEchoOutputs() = 0;
    [[nodiscard]] virtual std::string micDefaultOutput() const = 0;

    // Delay the chain adds, in milliseconds.
    [[nodiscard]] virtual float micLatencyMs() const = 0;
};

}  // namespace sonar::audio
