#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonar::audio {

// Broad hardware class of an output device, inferred from its node name.
enum class DeviceType { Usb, Bluetooth, Hdmi, Spdif, Analog, System, Other };

// A connected audio output (a real Audio/Sink, never one of our virtual
// sonero_* channel sinks).
struct AudioDevice {
    std::uint32_t nodeId = 0;      // registry id of the Audio/Sink node
    std::uint32_t deviceId = 0;    // registry id of the owning PW Device (0 if
                                   // none) — used later for BT codec / profiles
    std::int32_t routeDevice = -1; // card.profile.device: which of the device's
                                   // routes this sink is, for its volume
    std::uint64_t serial = 0;      // object.serial of the sink node (routing target)
    std::string name;              // node.name (stable identifier)
    std::string description;       // human-friendly label
    DeviceType type = DeviceType::Other;  // inferred from node.name
};

// One selectable A2DP codec, exposed by a Bluetooth device as a card profile.
struct DeviceCodec {
    std::uint32_t profileIndex = 0;  // SPA profile index that activates it
    std::string name;                // profile name, e.g. "a2dp-sink-aptx_hd"
    std::string codec;               // human label, e.g. "aptX HD"
    bool available = true;           // false if the peer can't negotiate it
};

// The A2DP codecs a Bluetooth output offers, plus which one is currently active.
struct DeviceCodecs {
    std::vector<DeviceCodec> codecs;
    std::uint32_t currentIndex = 0;
    bool valid = false;              // false when the device has no A2DP profiles
};

// Live view of the connected output devices. Backed by the PipeWire registry, so
// devices appear/disappear as hardware is plugged, unplugged, or (dis)connected.
class IAudioDevices {
public:
    virtual ~IAudioDevices() = default;

    // Snapshot of the currently-connected output devices, ascending by type then
    // description. Cheap: reads a cached model, no round-trip.
    [[nodiscard]] virtual std::vector<AudioDevice> outputDevices() = 0;

    // Monotonic counter bumped whenever the output-device set changes. The UI
    // polls this and only rebuilds when it moves.
    [[nodiscard]] virtual std::uint64_t devicesRevision() const = 0;

    // A Bluetooth device's A2DP codecs and the active one (`deviceId` is
    // AudioDevice::deviceId); invalid for anything else.
    [[nodiscard]] virtual DeviceCodecs deviceCodecs(std::uint32_t deviceId) = 0;

    // Switch a Bluetooth device to a codec by its profile index. Returns false if
    // the device could not be reached.
    virtual bool setDeviceProfile(std::uint32_t deviceId, std::uint32_t profileIndex) = 0;

    // A device sink's output volume (0..1), on the desktop slider's cubic scale and set
    // on the route, as the desktop does, so it survives a reboot. 1.0 when unreadable.
    [[nodiscard]] virtual float deviceVolume(std::uint32_t nodeId) = 0;
    virtual void setDeviceVolume(std::uint32_t nodeId, float volume) = 0;
};

}  // namespace sonar::audio
