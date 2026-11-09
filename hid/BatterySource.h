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

// A way of learning an audio output's battery level. Sonero asks every source about
// every output and takes the first answer. The kernel's power_supply class and BlueZ
// cover most wireless hardware; a vendor source covers hardware that reports only
// over its own HID protocol.
class IBatterySource {
public:
    virtual ~IBatterySource() = default;

    // For the Devices page, and for logs when a device reports nothing.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // The battery of the output with this node.name. nullopt when this source cannot
    // speak for the device; a reading with percent == -1 when it can but does not know.
    [[nodiscard]] virtual std::optional<BatteryReading> read(const std::string& nodeName) = 0;
};

// Every source this build has, in the order they are asked: generic first, so a
// device the kernel already reports is never routed through a vendor protocol.
[[nodiscard]] std::vector<std::unique_ptr<IBatterySource>> defaultBatterySources();

// Ask each source in turn and return the first that can speak for the device.
[[nodiscard]] std::optional<BatteryReading> readBattery(
    const std::vector<std::unique_ptr<IBatterySource>>& sources, const std::string& nodeName);

}  // namespace sonar::hid
