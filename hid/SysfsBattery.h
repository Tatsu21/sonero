#pragma once

#include "hid/BatterySource.h"

namespace sonar::hid {

// Battery from the kernel's own power_supply class — the generic path, and the
// one that should answer for most wireless hardware.
//
// The kernel exposes a charge level for any HID device whose driver reports one
// (hid-logitech-hidpp, hid-steelseries, the generic HID battery quirk, and
// others) as /sys/class/power_supply/<something>/capacity. Nothing vendor
// specific is needed to read it, and no list of products is involved.
//
// The work is in deciding which audio output a power_supply entry belongs to.
// Two signals are tried, because neither covers everything:
//
//  - The USB device both the entry and the audio node descend from. Walk up from
//    the power supply to it, read the `manufacturer` and `product` strings the
//    kernel got from its descriptors, and look for them in the audio node's
//    name — which ALSA built from those same two strings.
//
//  - The entry's own `model_name` and `manufacturer`. For anything behind a
//    receiver the USB strings describe the dongle, not the device: a Logitech
//    G305 on a Lightspeed receiver reports product "USB Receiver", while the
//    power supply names the mouse. Checked against real hardware, which is how
//    the first signal alone was found to be too narrow.
//
// Either matching means one physical device. Neither involves a list of
// products.
class SysfsBatterySource final : public IBatterySource {
public:
    [[nodiscard]] std::string_view name() const override { return "kernel power_supply"; }
    [[nodiscard]] std::optional<BatteryReading> read(const std::string& nodeName) override;

    // Exposed for testing: turn a USB descriptor string into the form ALSA uses
    // when it builds a node name ("SteelSeries Arctis Nova Pro" ->
    // "SteelSeries_Arctis_Nova_Pro").
    [[nodiscard]] static std::string alsaIdForm(const std::string& descriptorString);

    // Exposed for testing: does `nodeName` look like it came from this
    // manufacturer and product pair? Used for both signals above — the USB
    // descriptor strings and the power supply's own model_name.
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
