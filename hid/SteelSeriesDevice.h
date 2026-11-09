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

// What the base station reports about the headset it is paired with.
enum class HeadsetState {
    Unknown,
    Offline,   // powered off or out of range — no meaningful battery reading
    Charging,  // sitting on the dock / charging over cable
    Online,
};

// Battery status of the Nova Pro Wireless headset; -1 when unknown or offline. The
// base station reports only the headset's battery, not the spare in the dock.
struct BatteryStatus {
    bool valid = false;
    int headsetPercent = -1;
    HeadsetState state = HeadsetState::Unknown;
    std::array<std::uint8_t, 64> raw{};  // raw status report, for calibration
};

// The SteelSeries Arctis Nova Pro Wireless base station over raw hidraw (usage page
// 0xFFC0, report id 6, 64-byte reports).
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
