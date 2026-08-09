#include "audio/DeviceFormat.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>

#include "core/Log.h"

namespace sonar::audio {

namespace {
namespace fs = std::filesystem;

// Per-device drop-in filename so each output keeps its own pinned format.
std::string fileNameFor(const std::string& pattern) {
    std::string key;
    for (const char c : pattern) {
        key += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    }
    if (key.size() > 80) {
        key.resize(80);
    }
    return "51-sonero-format-" + key + ".conf";
}

// The SPA audio format name for a bit depth. The ALSA layer maps e.g. S24LE
// onto the device's packed S24_3LE transparently.
const char* spaFormatFor(int bitDepth) {
    switch (bitDepth) {
        case 16: return "S16LE";
        case 32: return "S32LE";
        default: return "S24LE";  // 24-bit
    }
}

fs::path configDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg) / "wireplumber" / "wireplumber.conf.d";
    }
    const char* home = std::getenv("HOME");
    return fs::path(home != nullptr ? home : ".") / ".config" / "wireplumber" /
           "wireplumber.conf.d";
}
}  // namespace

OutputFormatManager::OutputFormatManager(std::string nodeNamePattern)
    : nodePattern_(std::move(nodeNamePattern)) {}

const std::vector<int>& OutputFormatManager::supportedRates() {
    // Full standard PCM rate ladder — offered when the device cannot be queried
    // live. Mirrors kStdRates in PipeWireManager's EnumFormat collector.
    static const std::vector<int> rates = {44100, 48000, 88200, 96000, 176400, 192000};
    return rates;
}

const std::vector<int>& OutputFormatManager::supportedDepths() {
    static const std::vector<int> depths = {16, 24, 32};
    return depths;
}

std::string OutputFormatManager::configPath() const {
    return (configDir() / fileNameFor(nodePattern_)).string();
}

std::string OutputFormatManager::renderConfig(const OutputFormat& fmt) const {
    return std::format(
        "# Managed by Sonero — output format for the SteelSeries base station.\n"
        "# Pins the ALSA open format so the device emits at the chosen rate/bit depth.\n"
        "# Edits here are overwritten when you change the format in the app.\n"
        "monitor.alsa.rules = [\n"
        "  {{\n"
        "    matches = [ {{ node.name = \"{0}\" }} ]\n"
        "    actions = {{ update-props = {{ audio.format = \"{1}\"  audio.rate = {2} }} }}\n"
        "  }}\n"
        "]\n",
        nodePattern_, spaFormatFor(fmt.bitDepth), fmt.rateHz);
}

std::optional<OutputFormat> OutputFormatManager::current() const {
    std::ifstream in(configPath());
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    OutputFormat fmt;
    fmt.bitDepth = text.find("\"S16LE\"") != std::string::npos   ? 16
                   : text.find("\"S32LE\"") != std::string::npos ? 32
                                                                 : 24;
    if (const auto p = text.find("audio.rate"); p != std::string::npos) {
        const auto eq = text.find('=', p);
        if (eq != std::string::npos) {
            fmt.rateHz = std::atoi(text.c_str() + eq + 1);
        }
    }
    if (fmt.rateHz <= 0) {
        return std::nullopt;
    }
    return fmt;
}

bool OutputFormatManager::apply(const OutputFormat& fmt) {
    std::error_code ec;
    const fs::path dir = configDir();
    fs::create_directories(dir, ec);
    if (ec) {
        log::warn("Device format: cannot create {} ({})", dir.string(), ec.message());
        return false;
    }

    const std::string path = configPath();
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            log::warn("Device format: cannot write {}", path);
            return false;
        }
        out << renderConfig(fmt);
    }
    // Deliberately no WirePlumber restart: a global restart drops Bluetooth links.
    // The drop-in is picked up when WirePlumber next loads its config (next login
    // or a manual `systemctl --user restart wireplumber`).
    log::info("Device format: pinned {} bit / {} Hz via {}", fmt.bitDepth, fmt.rateHz, path);
    return true;
}

}  // namespace sonar::audio
