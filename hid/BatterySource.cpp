#include "hid/BatterySource.h"

#include "hid/SteelSeriesBattery.h"
#include "hid/SysfsBattery.h"

namespace sonar::hid {

std::vector<std::unique_ptr<IBatterySource>> defaultBatterySources() {
    std::vector<std::unique_ptr<IBatterySource>> sources;
    // Generic first. A device the kernel already reports should never be routed
    // through a vendor protocol: the kernel's answer needs no permissions beyond
    // reading /sys, works while another process holds the HID interface, and
    // keeps working when the vendor changes its report format.
    sources.push_back(std::make_unique<SysfsBatterySource>());
    sources.push_back(std::make_unique<SteelSeriesBatterySource>());
    return sources;
}

std::optional<BatteryReading> readBattery(
    const std::vector<std::unique_ptr<IBatterySource>>& sources, const std::string& nodeName) {
    for (const std::unique_ptr<IBatterySource>& source : sources) {
        if (source == nullptr) {
            continue;
        }
        // The first source that can speak for the device wins, even if what it
        // says is "I do not know the level right now" — a source that owns the
        // device is a better authority on that than the next one guessing.
        if (std::optional<BatteryReading> reading = source->read(nodeName)) {
            return reading;
        }
    }
    return std::nullopt;
}

}  // namespace sonar::hid
