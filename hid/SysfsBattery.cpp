#include "hid/SysfsBattery.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace sonar::hid {

namespace {

namespace fs = std::filesystem;

// First line of a sysfs file, trimmed. Missing or unreadable reads as empty,
// which every caller here treats as "this device does not say".
std::string readLine(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

ChargeState parseState(const std::string& status) {
    if (status == "Charging") return ChargeState::Charging;
    if (status == "Full") return ChargeState::Full;
    if (status == "Discharging" || status == "Not charging") return ChargeState::Discharging;
    return ChargeState::Unknown;
}

// Walk up from a sysfs device directory looking for the USB device that owns it
// — the one carrying `manufacturer` and `product`, which is what ALSA names the
// sound card after. Bounded, because sysfs symlink chains can be long and a
// wrong turn should end rather than wander.
fs::path findUsbAncestor(fs::path start) {
    std::error_code ec;
    for (int depth = 0; depth < 12 && !start.empty(); ++depth) {
        if (fs::exists(start / "manufacturer", ec) && fs::exists(start / "product", ec)) {
            return start;
        }
        const fs::path parent = start.parent_path();
        if (parent == start) {
            break;
        }
        start = parent;
    }
    return {};
}

}  // namespace

std::string SysfsBatterySource::root_ = "/sys/class/power_supply";

void SysfsBatterySource::setSysfsRoot(std::string root) { root_ = std::move(root); }

std::string SysfsBatterySource::alsaIdForm(const std::string& descriptorString) {
    // ALSA builds a card id from the USB strings by replacing anything that is
    // not alphanumeric with an underscore, then collapsing runs of them. Doing
    // the same here is what lets the two be compared at all.
    std::string out;
    out.reserve(descriptorString.size());
    for (const char c : descriptorString) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) != 0;
        if (keep) {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

bool SysfsBatterySource::nodeMatchesUsb(const std::string& nodeName,
                                        const std::string& manufacturer,
                                        const std::string& product) {
    if (nodeName.empty()) {
        return false;
    }
    // The product string alone is the reliable part: ALSA truncates long ids, and
    // some descriptors repeat the manufacturer inside the product ("SteelSeries
    // SteelSeries Arctis..."), so requiring both in order fails on real hardware.
    const std::string wantProduct = alsaIdForm(product);
    if (wantProduct.empty()) {
        return false;
    }
    if (nodeName.find(wantProduct) != std::string::npos) {
        return true;
    }
    // ...and when the product id was truncated, the manufacturer plus the start
    // of the product is still a strong enough signal to accept.
    const std::string wantVendor = alsaIdForm(manufacturer);
    if (wantVendor.size() >= 3 && nodeName.find(wantVendor) != std::string::npos) {
        const std::string head = wantProduct.substr(0, std::min<std::size_t>(8, wantProduct.size()));
        return head.size() >= 4 && nodeName.find(head) != std::string::npos;
    }
    return false;
}

std::optional<BatteryReading> SysfsBatterySource::read(const std::string& nodeName) {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        return std::nullopt;  // no power_supply class at all
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(root_, ec)) {
        const fs::path dir = entry.path();

        // Peripherals only. A laptop's own battery and its mains adapter also
        // live here, and reporting the laptop's charge as a headset's would be
        // worse than reporting nothing.
        const std::string scope = readLine(dir / "scope");
        if (scope != "Device") {
            continue;
        }

        // Signal one: the USB device this power supply hangs off.
        bool matched = false;
        const fs::path device = fs::canonical(dir / "device", ec);
        if (!ec) {
            if (const fs::path usb = findUsbAncestor(device); !usb.empty()) {
                matched = nodeMatchesUsb(nodeName, readLine(usb / "manufacturer"),
                                         readLine(usb / "product"));
            }
        }
        ec.clear();

        // Signal two: what the power supply calls itself. For anything behind a
        // receiver the USB strings describe the dongle, so this is the only one
        // that identifies the device.
        if (!matched) {
            matched = nodeMatchesUsb(nodeName, readLine(dir / "manufacturer"),
                                     readLine(dir / "model_name"));
        }
        if (!matched) {
            continue;
        }

        const std::string capacity = readLine(dir / "capacity");
        BatteryReading reading;
        reading.source = std::string(name());
        reading.state = parseState(readLine(dir / "status"));
        if (!capacity.empty()) {
            try {
                reading.percent = std::clamp(std::stoi(capacity), 0, 100);
            } catch (const std::exception&) {
                reading.percent = -1;  // present but not saying, which is a reading
            }
        }
        return reading;
    }
    return std::nullopt;
}

}  // namespace sonar::hid
