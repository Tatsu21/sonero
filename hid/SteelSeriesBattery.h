#pragma once

#include "hid/BatterySource.h"
#include "hid/SteelSeriesDevice.h"

namespace sonar::hid {

// Vendor battery source for hardware that reports only over its own HID protocol.
// A second vendor is another file like this plus a line in defaultBatterySources().
class SteelSeriesBatterySource final : public IBatterySource {
public:
    [[nodiscard]] std::string_view name() const override { return "SteelSeries HID"; }
    [[nodiscard]] std::optional<BatteryReading> read(const std::string& nodeName) override;

private:
    SteelSeriesDevice device_;
};

}  // namespace sonar::hid
