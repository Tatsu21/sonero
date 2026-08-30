#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QList>
#include <QString>
#include <QWidget>

#include "audio/DeviceFormat.h"
#include "dsp/Equalizer.h"
#include "hid/SteelSeriesDevice.h"

class QComboBox;
class QLabel;
class QProgressBar;
class QSlider;
class QTimer;
class QVBoxLayout;

namespace sonar::audio {
class IDeviceFormats;
class IAudioDevices;
struct AudioDevice;
}  // namespace sonar::audio

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class EqCurve;
class Notifier;

// One connected device's battery, as the tray menu shows it. Reported by
// DevicesPage, which already polls both sources (USB HID and BlueZ).
struct BatteryLevel {
    QString device;
    int percent = -1;
    bool charging = false;

    bool operator==(const BatteryLevel&) const = default;
};

// Lists every connected audio output (USB, Bluetooth, HDMI, S/PDIF, analog) live
// — devices appear and disappear as they are plugged / unplugged. The SteelSeries
// Arctis Nova Pro base station additionally gets its onboard controls (battery,
// transmission format, and a Sonar-style HID equalizer).
class DevicesPage : public QWidget {
    Q_OBJECT

public:
    explicit DevicesPage(audio::IAudioDevices* devices = nullptr,
                         audio::IDeviceFormats* formats = nullptr,
                         config::SettingsStore* settings = nullptr, Notifier* notifier = nullptr,
                         QWidget* parent = nullptr);

    // The levels as of the last poll. The first poll happens inside the
    // constructor, before any caller can connect to batteriesChanged(), and the
    // signal then stays quiet until something changes — so a listener has to seed
    // itself from this rather than wait for an edge that already went by.
    [[nodiscard]] const QList<BatteryLevel>& batteries() const { return lastBatteries_; }

signals:
    // Emitted only when a level actually changes, so a listener can rebuild a
    // menu without doing it every 1.5 s poll.
    void batteriesChanged(const QList<BatteryLevel>& levels);

private:
    // Rebuilds the connected-outputs list when the device set changes; keeps the
    // SteelSeries controls shown only while the base station is present.
    void syncDevices();
    void buildDeviceRow(QVBoxLayout* body, const audio::AudioDevice& dev);
    void buildSteelSeriesControls(QVBoxLayout* parent);
    // Refreshes the row's badge and hands back the level it found, if any.
    std::optional<int> updateBtBattery(QLabel* label, const std::string& nodeName);
    void publishBatteries(QList<BatteryLevel> levels);  // headset first, then Bluetooth

    void refresh();  // SteelSeries battery / status
    void onEqBandChanged(int index, float gainDb);
    void applyHeadsetPreset(const std::array<int, 10>& gainsDb);
    void sendHeadsetEq();
    void buildFormatCard(QVBoxLayout* root);
    void syncFormatDevices();            // refill the ALSA-device selector
    void loadFormatForSelectedDevice();  // fill rate/depth for the chosen device
    void applyFormat();
    void setBand(int index, float gainDb);  // update model + both widgets + queue send
    void saveHeadsetEq();                    // persist the headset EQ to the store

    audio::IAudioDevices* devices_ = nullptr;
    audio::IDeviceFormats* formats_ = nullptr;
    config::SettingsStore* settings_ = nullptr;
    Notifier* notifier_ = nullptr;
    hid::SteelSeriesDevice device_;

    // Notification bookkeeping (avoid spamming; edge-triggered only).
    std::unordered_map<std::string, std::string> knownDevices_;  // name -> description
    bool devicesInitialized_ = false;                            // skip the first sync
    std::unordered_set<std::string> btLowWarned_;                // BT names warned low
    bool headsetLowWarned_ = false;

    QVBoxLayout* deviceListBody_ = nullptr;   // rows, one per connected output
    QWidget* steelSeriesCard_ = nullptr;      // battery + EQ (hidden when absent)
    std::uint64_t lastDevicesRev_ = ~0ULL;    // force the initial build
    QTimer* timer_ = nullptr;
    // Bluetooth outputs whose battery is polled each tick.
    struct BtBattery {
        QLabel* label;            // the row's "\U0001f50b 78%" badge
        std::string nodeName;     // PipeWire node.name; keys the BlueZ lookup
        std::string description;  // human name, for the tray menu
    };
    std::vector<BtBattery> btBatteries_;
    // Bluetooth outputs already switched to their best codec on connect (keyed by
    // node.name). Pruned when a device disappears so a reconnect re-applies.
    std::unordered_set<std::string> autoPreferredDevices_;

    QComboBox* formatDeviceCombo_ = nullptr;  // which ALSA output to format
    QComboBox* rateCombo_ = nullptr;
    QComboBox* depthCombo_ = nullptr;
    QLabel* formatStatus_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* headsetBar_ = nullptr;
    QLabel* headsetPct_ = nullptr;
    int headsetPercent_ = -1;        // last reading; -1 when the dock is absent
    bool headsetCharging_ = false;
    QList<BatteryLevel> lastBatteries_;  // what was last emitted, to skip no-ops
    QLabel* rawLabel_ = nullptr;

    EqCurve* eqCurve_ = nullptr;
    dsp::EqSettings headsetEq_;
    std::array<QSlider*, 10> eqSliders_{};
    std::array<QLabel*, 10> eqValues_{};
    QTimer* eqTimer_ = nullptr;
};

}  // namespace sonar::ui
