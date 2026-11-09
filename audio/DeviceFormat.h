#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sonar::audio {

// A hardware output format: the sample rate the graph runs at for the device and
// the bit depth the ALSA PCM is opened with.
struct OutputFormat {
    int rateHz = 48000;   // 44100 .. 192000
    int bitDepth = 24;    // 16, 24, or 32

    friend bool operator==(const OutputFormat& a, const OutputFormat& b) {
        return a.rateHz == b.rateHz && a.bitDepth == b.bitDepth;
    }
};

// Pins the rate / bit depth of one ALSA output via a per-device WirePlumber
// drop-in, applied when WirePlumber next loads its config (Sonero never restarts
// it: that drops Bluetooth). There is no live param for the hardware format, and
// ALSA clamps rates the device does not support. Bluetooth is not managed here.
class OutputFormatManager {
public:
    // `nodeNamePattern` is a WirePlumber match value (a leading '~' makes it a
    // regex). Defaults to the SteelSeries Arctis Nova Pro Wireless output.
    explicit OutputFormatManager(
        std::string nodeNamePattern =
            "~alsa_output.usb-SteelSeries_Arctis_Nova_Pro_Wireless.*");

    // Sample rates / bit depths offered in the UI.
    [[nodiscard]] static const std::vector<int>& supportedRates();
    [[nodiscard]] static const std::vector<int>& supportedDepths();

    // Absolute path of the managed WirePlumber drop-in.
    [[nodiscard]] std::string configPath() const;

    // The format pinned by a previously-written drop-in, or nullopt if none.
    [[nodiscard]] std::optional<OutputFormat> current() const;

    // Write the per-device drop-in pinning `fmt`; false if it could not be written.
    // Applies at the next WirePlumber load, never by restarting it.
    bool apply(const OutputFormat& fmt);

    // The SPA-JSON body written to the drop-in for `fmt` and the node pattern.
    // Exposed for testing; pure, no side effects.
    [[nodiscard]] std::string renderConfig(const OutputFormat& fmt) const;

private:
    std::string nodePattern_;
};

}  // namespace sonar::audio
