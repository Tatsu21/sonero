#include "hid/BatterySource.h"

#include "hid/SteelSeriesBattery.h"
#include "hid/SysfsBattery.h"

namespace sonar::hid {

std::vector<std::unique_ptr<IBatterySource>> defaultBatterySources() {
    std::vector<std::unique_ptr<IBatterySource>> sources;
    // Generic first: the kernel's answer needs no permissions, works while another
    // process holds the HID interface, and survives vendor format changes.
    sources.push_back(std::make_unique<SysfsBatterySource>());
    sources.push_back(std::make_unique<SteelSeriesBatterySource>());
    return sources;
}

std::optional<BatteryReading> readBattery(
    const std::vector<std::unique_ptr<IBatterySource>>& sources, const std::string& nodeName) {
    for (const std::unique_ptr<IBatterySource>& source : sources) {
        if (source == nullptr) continue;
        // The first source that speaks for the device wins, even with "level unknown".
        if (std::optional<BatteryReading> reading = source->read(nodeName)) return reading;
    }
    return std::nullopt;
}

}  // namespace sonar::hid
