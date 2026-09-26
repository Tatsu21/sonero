#pragma once

#include <string>
#include <string_view>

#include "audio/MicProcessor.h"  // MicLevels
#include "dsp/MicChain.h"

namespace sonar::audio {

// The microphone's processing chain, as the UI sees it.
//
// Separate from IChannelController because the microphone is not a channel with
// a fader on it: it is a signal path with stages that can each be switched on,
// each reporting how hard they are working. Implemented by the backend, which
// owns the node the chain runs in.
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

    // The real input device the chain pulls from; empty means nothing attached.
    virtual bool setMicCaptureDevice(const std::string& nodeName) = 0;
    [[nodiscard]] virtual std::string micCaptureDevice() const = 0;

    // Delay the chain adds, in milliseconds.
    [[nodiscard]] virtual float micLatencyMs() const = 0;
};

}  // namespace sonar::audio
