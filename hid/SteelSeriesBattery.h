#pragma once

#include "hid/BatterySource.h"
#include "hid/SteelSeriesDevice.h"

namespace sonar::hid {

// A vendor battery source, for hardware that reports its charge only over its
// own HID protocol and so is invisible to the kernel's power_supply class.
//
// It is deliberately shaped as one IBatterySource among several rather than as
// the battery feature itself: a second vendor costs another file like this one
// and a line in defaultBatterySources(), and nothing in the pages or the tray
// has to know it exists. This one happens to be SteelSeries because that is the
// hardware the author owns.
class SteelSeriesBatterySource final : public IBatterySource {
public:
    [[nodiscard]] std::string_view name() const override { return "SteelSeries HID"; }
    [[nodiscard]] std::optional<BatteryReading> read(const std::string& nodeName) override;

private:
    SteelSeriesDevice device_;
};

}  // namespace sonar::hid
