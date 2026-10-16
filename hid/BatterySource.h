#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sonar::hid {

enum class ChargeState { Unknown, Discharging, Charging, Full };

// One device's charge level, from whichever source could speak for it.
struct BatteryReading {
    int percent = -1;  // -1 when the level is unknown (asleep, out of range)
    ChargeState state = ChargeState::Unknown;
    std::string source;  // which backend answered, for the Devices page to show

    [[nodiscard]] bool valid() const { return percent >= 0; }
    [[nodiscard]] bool charging() const {
        return state == ChargeState::Charging || state == ChargeState::Full;
    }
};

// A way of learning the battery level of an audio output.
//
// This exists so that battery support is not a list of blessed products. Sonero
// asks every source it has about every output it can see and takes the first
// answer. Two of the sources are fully generic — the kernel's own power_supply
// class, and BlueZ — and between them they cover any device whose driver or
// protocol already reports a charge level, which is most wireless hardware. A
// vendor source is for hardware that reports battery only over its own HID
// protocol; it is one backend among several rather than the centre of the
// feature, and adding another vendor costs one more implementation of this
// interface and nothing else.
class IBatterySource {
public:
    virtual ~IBatterySource() = default;

    // For the Devices page, and for logs when a device reports nothing.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // The battery of the audio output with this PipeWire node.name. Returns
    // nullopt when this source cannot speak for that device at all, which is
    // different from "it can, and the level is unknown" — that comes back as a
    // reading with percent == -1.
    [[nodiscard]] virtual std::optional<BatteryReading> read(const std::string& nodeName) = 0;
};

// Every source this build has, in the order they are asked: generic first, so a
// device the kernel already reports is never routed through a vendor protocol.
[[nodiscard]] std::vector<std::unique_ptr<IBatterySource>> defaultBatterySources();

// Ask each source in turn and return the first that can speak for the device.
[[nodiscard]] std::optional<BatteryReading> readBattery(
    const std::vector<std::unique_ptr<IBatterySource>>& sources, const std::string& nodeName);

}  // namespace sonar::hid
