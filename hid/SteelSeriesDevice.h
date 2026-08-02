#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonar::hid {

// Result of a permission-free probe (reads /sys only, never opens the device).
struct ProbeResult {
    bool present = false;     // the device is plugged in
    bool accessible = false;  // we could open its control interface
    std::string path;         // /dev/hidrawN of the control interface
    std::string error;        // why it is not accessible (e.g. permission)
};

// Dual-battery status of the Nova Pro Wireless (one battery in the headset, one
// charging in the dock). Percent is -1 when unknown.
struct BatteryStatus {
    bool valid = false;
    int headsetPercent = -1;
    int sparePercent = -1;  // spare battery charging in the base station
    std::array<std::uint8_t, 64> raw{};  // raw status report, for calibration
};

// Talks to the SteelSeries Arctis Nova Pro Wireless base station over its
// vendor HID control interface (usage page 0xFFC0, report id 6, 64-byte reports)
// using raw hidraw — no external dependency.
class SteelSeriesDevice {
public:
    static constexpr std::uint16_t kVendorId = 0x1038;
    static constexpr std::uint16_t kNovaProWireless = 0x12e0;
    static constexpr std::size_t kReportSize = 64;  // report id + 63 bytes

    SteelSeriesDevice() = default;
    ~SteelSeriesDevice();

    SteelSeriesDevice(const SteelSeriesDevice&) = delete;
    SteelSeriesDevice& operator=(const SteelSeriesDevice&) = delete;

    // Locate and open the control interface. Returns false if not found or not
    // permitted (see probe() for the reason).
    bool open();
    void close();
    [[nodiscard]] bool isOpen() const { return fd_ >= 0; }

    // Detection without opening the device.
    [[nodiscard]] static ProbeResult probe();

    // Request + parse the dual battery status (sends {0x06, 0xb0}).
    [[nodiscard]] BatteryStatus readBattery();

    // Send a 10-band EQ to the headset's onboard EQ. Gains are in dB (about
    // -10..+10); encoded as byte = 0x14 + gain*2 (0x14 = 0 dB). Commits with a
    // save command after a short delay.
    bool setEqualizer(const std::array<int, 10>& gainsDb);

    // Low-level HID I/O. `report[0]` is the report id.
    bool writeReport(const std::vector<std::uint8_t>& report);
    // Reads one input report (blocks up to timeoutMs); returns bytes read, or -1.
    int readReport(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs);

private:
    // Scans /sys/class/hidraw for the 1038:12e0 interface with usage page 0xFFC0.
    [[nodiscard]] static std::string findControlPath();

    int fd_ = -1;
};

}  // namespace sonar::hid
