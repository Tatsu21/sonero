#include "hid/SteelSeriesDevice.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "core/Log.h"

namespace sonar::hid {

namespace {
namespace fs = std::filesystem;

std::vector<std::uint8_t> readAllBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string readText(const fs::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// The vendor control collection begins with Usage Page (0xFFC0): bytes 06 c0 ff.
bool hasVendorControlPage(const std::vector<std::uint8_t>& desc) {
    for (std::size_t i = 0; i + 2 < desc.size(); ++i) {
        if (desc[i] == 0x06 && desc[i + 1] == 0xc0 && desc[i + 2] == 0xff) {
            return true;
        }
    }
    return false;
}

bool matchesVidPid(const std::string& uevent) {
    // HID_ID=0003:00001038:000012E0
    return uevent.find("00001038:000012E0") != std::string::npos ||
           uevent.find("00001038:000012e0") != std::string::npos;
}
}  // namespace

SteelSeriesDevice::~SteelSeriesDevice() { close(); }

std::string SteelSeriesDevice::findControlPath() {
    std::error_code ec;
    const fs::path base("/sys/class/hidraw");
    if (!fs::exists(base, ec)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        const fs::path dev = entry.path() / "device";
        if (!matchesVidPid(readText(dev / "uevent"))) {
            continue;
        }
        if (!hasVendorControlPage(readAllBytes(dev / "report_descriptor"))) {
            continue;
        }
        return "/dev/" + entry.path().filename().string();
    }
    return {};
}

ProbeResult SteelSeriesDevice::probe() {
    ProbeResult result;
    result.path = findControlPath();
    result.present = !result.path.empty();
    if (!result.present) {
        result.error = "device not found";
        return result;
    }
    const int fd = ::open(result.path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        result.accessible = true;
        ::close(fd);
    } else {
        result.error = std::strerror(errno);
    }
    return result;
}

bool SteelSeriesDevice::open() {
    close();
    const std::string path = findControlPath();
    if (path.empty()) {
        return false;
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    return fd_ >= 0;
}

void SteelSeriesDevice::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

BatteryStatus SteelSeriesDevice::readBattery() {
    BatteryStatus status;
    if (fd_ < 0) {
        return status;
    }
    // Status request: report id 6, command 0xb0.
    std::vector<std::uint8_t> request(kReportSize, 0);
    request[0] = 0x06;
    request[1] = 0xb0;
    if (!writeReport(request)) {
        return status;
    }
    std::uint8_t buffer[kReportSize] = {};
    const int n = readReport(buffer, kReportSize, 250);
    if (n < 16 || buffer[0] != 0x06 || buffer[1] != 0xb0) {
        return status;
    }
    std::memcpy(status.raw.data(), buffer, kReportSize);

    // Byte 15 carries the link state, byte 6 the charge on a 0..8 scale. (Matches
    // the protocol as implemented by HeadsetControl for this model; an earlier
    // guess here read bytes 10/11, which only appeared correct by coincidence.)
    constexpr std::uint8_t kHeadsetOffline = 0x01;
    constexpr std::uint8_t kHeadsetCharging = 0x02;
    constexpr std::uint8_t kHeadsetOnline = 0x08;
    constexpr int kChargeMax = 8;

    switch (buffer[15]) {
        case kHeadsetOffline: status.state = HeadsetState::Offline; break;
        case kHeadsetCharging: status.state = HeadsetState::Charging; break;
        case kHeadsetOnline: status.state = HeadsetState::Online; break;
        default: status.state = HeadsetState::Unknown; break;
    }

    if (status.state != HeadsetState::Offline) {
        const int raw = std::min<int>(buffer[6], kChargeMax);
        // Eight steps of 12.5%, rounded — the hardware has no finer resolution.
        status.headsetPercent = (raw * 100 + kChargeMax / 2) / kChargeMax;
    }
    status.valid = true;
    return status;
}

bool SteelSeriesDevice::setEqualizer(const std::array<int, 10>& gainsDb) {
    if (fd_ < 0) {
        log::warn("Headset EQ: device not open");
        return false;
    }
    // Handshake: the reference sequence that worked read the status right before
    // sending the EQ, so replicate that (request + drain the reply).
    std::vector<std::uint8_t> statusReq(kReportSize, 0);
    statusReq[0] = 0x06;
    statusReq[1] = 0xb0;
    writeReport(statusReq);
    std::uint8_t drain[kReportSize] = {};
    readReport(drain, kReportSize, 100);

    std::vector<std::uint8_t> msg(kReportSize, 0);
    msg[0] = 0x06;
    msg[1] = 0x33;  // equalizer command
    for (std::size_t i = 0; i < gainsDb.size(); ++i) {
        int byte = 0x14 + gainsDb[i] * 2;  // 0x14 = 0 dB, ±10 dB -> 0x00..0x28
        byte = byte < 0x00 ? 0x00 : (byte > 0x28 ? 0x28 : byte);
        msg[2 + i] = static_cast<std::uint8_t>(byte);
    }
    const bool wroteEq = writeReport(msg);
    log::info("Headset EQ: sent 0x33 [{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} "
              "{:02x} {:02x}] -> write {}",
              msg[2], msg[3], msg[4], msg[5], msg[6], msg[7], msg[8], msg[9], msg[10], msg[11],
              wroteEq ? "ok" : "FAILED");
    if (!wroteEq) {
        return false;
    }
    ::usleep(200000);  // the working reference used ~0.2 s before committing
    std::vector<std::uint8_t> save(kReportSize, 0);
    save[0] = 0x06;
    save[1] = 0x09;  // save/commit
    const bool wroteSave = writeReport(save);
    log::info("Headset EQ: sent 0x09 save -> write {}", wroteSave ? "ok" : "FAILED");
    return true;
}

bool SteelSeriesDevice::writeReport(const std::vector<std::uint8_t>& report) {
    if (fd_ < 0 || report.empty()) {
        return false;
    }
    const ssize_t n = ::write(fd_, report.data(), report.size());
    return n == static_cast<ssize_t>(report.size());
}

int SteelSeriesDevice::readReport(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs) {
    if (fd_ < 0) {
        return -1;
    }
    pollfd pfd{fd_, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr <= 0) {
        return pr;  // 0 = timeout, -1 = error
    }
    return static_cast<int>(::read(fd_, buffer, maxLen));
}

}  // namespace sonar::hid
