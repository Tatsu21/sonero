#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "hid/SteelSeriesDevice.h"

// Standalone probe: reports whether the SteelSeries control interface is present
// and whether we may open it (i.e. whether the udev rule is in effect).
int main(int argc, char** argv) {
    const bool watch = argc > 1 && std::strcmp(argv[1], "--watch") == 0;
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
                        "(packaging/udev/70-sonero-steelseries.rules)\n");
        }
        return 1;
    }

    // Read the battery a few times: the dock answers with a status report only
    // once it has something to say, so a single miss is not a failure.
    sonar::hid::SteelSeriesDevice device;
    if (!device.open()) {
        std::printf("  open:       FAILED\n");
        return 1;
    }
    std::printf("\nbattery reads:\n");
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const sonar::hid::BatteryStatus s = device.readBattery();
        std::printf("  [%d] valid=%s headset=%d%% state=%d  raw:", attempt,
                    s.valid ? "yes" : "NO ", s.headsetPercent, static_cast<int>(s.state));
        std::printf("\n");
        for (std::size_t i = 0; i < s.raw.size(); ++i) {
            if (i % 16 == 0) {
                std::printf("      [%02zu]", i);
            }
            std::printf(" %02x", s.raw[i]);
            if (i % 16 == 15) {
                std::printf("\n");
            }
        }
    }

    if (watch) {
        // Print a line only when the report actually changes, so you can power the
        // headset on/off or pull the spare battery and see exactly which byte moves.
        // That is how the decode offsets were calibrated in the first place.
        std::printf("\nwatching for changes (Ctrl-C to stop) — change headset state now:\n");
        std::array<std::uint8_t, 16> previous{};
        bool havePrevious = false;
        for (;;) {
            const sonar::hid::BatteryStatus s = device.readBattery();
            std::array<std::uint8_t, 16> current{};
            std::copy(s.raw.begin(), s.raw.begin() + 16, current.begin());
            if (!havePrevious || current != previous) {
                std::printf("  headset=%3d%% state=%d  raw:", s.headsetPercent,
                            static_cast<int>(s.state));
                for (std::size_t i = 0; i < current.size(); ++i) {
                    // Mark the bytes that changed since the last report.
                    const bool changed = havePrevious && current[i] != previous[i];
                    std::printf(" %s%02x%s", changed ? "[" : "", current[i], changed ? "]" : "");
                }
                std::printf("\n");
                std::fflush(stdout);
                previous = current;
                havePrevious = true;
            }
            usleep(1000000);
        }
    }
    return 0;
}
