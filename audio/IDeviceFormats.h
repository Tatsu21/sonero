#pragma once

#include <string>
#include <vector>

namespace sonar::audio {

// The sample rates and bit depths an output device advertises to the host.
struct DeviceFormats {
    std::vector<int> rates;   // Hz, ascending (e.g. 44100, 48000, 96000)
    std::vector<int> depths;  // bit depths, ascending (16, 24, 32)
    bool valid = false;       // false when the device could not be queried
};

// Queries the formats a hardware output node supports (its SPA_PARAM_EnumFormat).
class IDeviceFormats {
public:
    virtual ~IDeviceFormats() = default;

    // Returns the formats advertised by the first Audio/Sink node whose node.name
    // contains `nameContains`. `valid` is false if no such node exists or the
    // backend is offline.
    [[nodiscard]] virtual DeviceFormats supportedFormats(const std::string& nameContains) = 0;
};

}  // namespace sonar::audio
