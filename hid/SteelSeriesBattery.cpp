#include "hid/SteelSeriesBattery.h"

namespace sonar::hid {

std::optional<BatteryReading> SteelSeriesBatterySource::read(const std::string& nodeName) {
    // The base station is one USB device presenting both an audio interface and
    // a vendor HID control interface, so its audio node name is the hook: only
    // speak for a device that looks like it.
    if (nodeName.find("SteelSeries") == std::string::npos) {
        return std::nullopt;
    }
    if (!SteelSeriesDevice::probe().present) {
        return std::nullopt;
    }
    if (!device_.isOpen() && !device_.open()) {
        return std::nullopt;  // present but not readable (udev rule not installed)
    }
    const BatteryStatus status = device_.readBattery();
    if (!status.valid) {
        return std::nullopt;
    }

    BatteryReading reading;
    reading.source = std::string(name());
    reading.percent = status.headsetPercent;
    switch (status.state) {
        case HeadsetState::Charging: reading.state = ChargeState::Charging; break;
        case HeadsetState::Online:   reading.state = ChargeState::Discharging; break;
        case HeadsetState::Offline:
        case HeadsetState::Unknown:  reading.state = ChargeState::Unknown; break;
    }
    return reading;
}

}  // namespace sonar::hid
