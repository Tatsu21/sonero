#include <cstdio>

#include "hid/SteelSeriesDevice.h"

// Standalone probe: reports whether the SteelSeries control interface is present
// and whether we may open it (i.e. whether the udev rule is in effect).
int main() {
    const sonar::hid::ProbeResult r = sonar::hid::SteelSeriesDevice::probe();
    std::printf("SteelSeries Arctis Nova Pro Wireless (1038:12e0)\n");
    std::printf("  present:    %s\n", r.present ? "yes" : "no");
    std::printf("  path:       %s\n", r.path.empty() ? "(none)" : r.path.c_str());
    std::printf("  accessible: %s\n", r.accessible ? "yes" : "no");
    if (!r.accessible && !r.error.empty()) {
        std::printf("  reason:     %s\n", r.error.c_str());
        if (!r.present) {
            std::printf("  hint:       make sure the base station is plugged in\n");
        } else {
            std::printf("  hint:       install the udev rule "
                        "(packaging/udev/70-linuxsonar-steelseries.rules)\n");
        }
    }
    return r.accessible ? 0 : 1;
}
