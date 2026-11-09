#pragma once

#include "hid/BatterySource.h"

namespace sonar::hid {

// Battery from the kernel's power_supply class (/sys/class/power_supply/*/capacity),
// the generic path for any HID driver that reports a charge. An entry belongs to an
// audio output when either
//  - the USB device both descend from has `manufacturer` / `product` strings found
//    in the audio node's name (ALSA builds it from them), or
//  - the entry's own `model_name` / `manufacturer` match — needed behind receivers,
//    where the USB strings describe the dongle (e.g. "USB Receiver").
class SysfsBatterySource final : public IBatterySource {
public:
    [[nodiscard]] std::string_view name() const override { return "kernel power_supply"; }
    [[nodiscard]] std::optional<BatteryReading> read(const std::string& nodeName) override;

    // For tests: a USB string in ALSA's node-name form ("SteelSeries Arctis Nova Pro" ->
    // "SteelSeries_Arctis_Nova_Pro").
    [[nodiscard]] static std::string alsaIdForm(const std::string& descriptorString);

    // For tests: does `nodeName` look like it came from this manufacturer / product?
    [[nodiscard]] static bool nodeMatchesUsb(const std::string& nodeName,
                                             const std::string& manufacturer,
                                             const std::string& product);

    // Where to look. Overridable so the tests can point it at a fixture tree
    // instead of the running machine's.
    static void setSysfsRoot(std::string root);

private:
    static std::string root_;
};

}  // namespace sonar::hid
