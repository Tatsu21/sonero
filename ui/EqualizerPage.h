#pragma once

#include <array>
#include <cstdint>

#include <QWidget>

#include "audio/Channel.h"
#include "dsp/Equalizer.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;

namespace sonar::audio {
class IEqualizerController;
class IAudioDevices;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class EqCurve;

// Per-channel graphic equalizer page: channel selector, preset picker (built-in
// and user-saved), band count (10/15/31) and an interactive response curve.
// Changes are applied to the real audio path through IEqualizerController.
class EqualizerPage : public QWidget {
    Q_OBJECT

public:
    explicit EqualizerPage(audio::IEqualizerController* controller,
                           config::SettingsStore* settings = nullptr,
                           audio::IAudioDevices* devices = nullptr, QWidget* parent = nullptr);

signals:
    // The Mixer page owns this state (its metering loop drives auto-gain), so the
    // controls here report intent rather than applying it directly.
    void channelGainChanged(audio::ChannelId id, float gainDb);
    void channelAutoGainToggled(audio::ChannelId id, bool on);
    void channelOutputChanged(audio::ChannelId id, const QString& deviceNodeName);
    void channelSelected(audio::ChannelId id);

public:
    // Which channel the page is showing, so the owner can seed the controls with
    // that channel's stored gain before the user touches anything.
    [[nodiscard]] audio::ChannelId selectedChannel() const;

public slots:
    // Reflect values the Mixer page holds, when the selection changes.
    void showChannelGain(float gainDb, bool autoOn);
    // Follow the auto-gain loop while it runs: it owns the value, so the slider
    // and the readout have to be told, not asked.
    void showAutoGainValue(audio::ChannelId id, float gainDb);

private:
    [[nodiscard]] dsp::EqSettings& current();
    void reflectControls(bool includeCombo);  // model -> controls
    void loadChannel();
    void refreshOutputs();  // refill the per-channel device selector
    void onBandChanged(int index, float gainDb);
    void pushEq();          // current channel -> real audio
    void restoreEq();       // load persisted per-channel EQ into the model
    void saveEq();          // snapshot every channel's EQ into the settings store

    void rebuildPresetCombo();
    void onPresetSelected();
    void onSavePreset();
    void onImportPreset();
    void onExportPreset();

    std::array<dsp::EqSettings, sonar::audio::kAllChannels.size()> eq_;
    int channel_ = 0;
    audio::IEqualizerController* controller_ = nullptr;
    audio::IAudioDevices* devices_ = nullptr;
    class QSlider* gain_ = nullptr;
    QCheckBox* autoGain_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QComboBox* output_ = nullptr;
    std::uint64_t devicesRevision_ = ~0ULL;
    config::SettingsStore* settings_ = nullptr;

    EqCurve* curve_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QButtonGroup* channelGroup_ = nullptr;
    QButtonGroup* bandGroup_ = nullptr;

    QTimer* eqFlush_ = nullptr;  // throttles audio updates while dragging
    bool eqDirty_ = false;
};

}  // namespace sonar::ui
