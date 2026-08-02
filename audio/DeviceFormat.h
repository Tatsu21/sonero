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

// Pins the sample rate / bit depth of one ALSA output node. Construct it with the
// exact device node.name so each output keeps its own drop-in; Bluetooth outputs
// have no ALSA format (their codec is the "format") and are not managed here.
//
// Hardware ALSA nodes pick their open format at negotiation time from the
// `audio.format` / `audio.rate` node properties, which WirePlumber applies from
// its monitor rules. There is no reliable *live* param to change this once the
// PCM is open (setting SPA_PARAM_PortConfig only reconfigures the graph side,
// not the hardware format). So we persist a per-device WirePlumber drop-in; it is
// honoured when WirePlumber next loads its config (next login or a manual
// restart). We do NOT restart WirePlumber ourselves — that drops Bluetooth links.
//
// Note: the rate is only honoured up to what the hardware advertises. The Nova
// Pro Wireless analog output only exposes 48 kHz, so requesting 96 kHz is
// clamped back to 48 kHz by ALSA — the option is offered for completeness.
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

    // Write (or overwrite) the per-device drop-in pinning `fmt`. Returns false if
    // the file could not be written. Intentionally does NOT restart WirePlumber
    // (a global restart drops Bluetooth); the format applies when WirePlumber next
    // loads its config (next login, or a manual restart).
    bool apply(const OutputFormat& fmt);

    // The SPA-JSON body written to the drop-in for `fmt` and the node pattern.
    // Exposed for testing; pure, no side effects.
    [[nodiscard]] std::string renderConfig(const OutputFormat& fmt) const;

private:
    std::string nodePattern_;
};

}  // namespace sonar::audio
