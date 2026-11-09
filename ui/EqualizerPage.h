#pragma once

#include <array>
#include <cstdint>

#include <QWidget>

#include <vector>

#include "audio/Channel.h"
#include "dsp/BassStage.h"
#include "dsp/Equalizer.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class QVBoxLayout;

namespace sonar::audio {
class IEqualizerController;
class IAudioDevices;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class EqCurve;

// Per-channel equalizer page: channel selector, presets (built-in and saved), band
// count (10/15/31) and the response curve, applied via IEqualizerController.
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
    // Which device this channel plays to; restored each time the combo is rebuilt.
    void showChannelOutput(const QString& deviceNodeName);
    // Follow the auto-gain loop while it runs: it owns the value, so the slider
    // and the readout have to be told, not asked.
    void showAutoGainValue(audio::ChannelId id, float gainDb);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void flushPending();  // pending EQ / bass edits and the output list
    [[nodiscard]] dsp::EqSettings& current();
    void reflectControls(bool includeCombo);  // model -> controls
    void loadChannel();
    void refreshOutputs();  // refill the per-channel device selector
    void onBandChanged(int index, float gainDb);
    void pushEq();          // current channel -> real audio
    void restoreEq();       // load persisted per-channel EQ into the model
    void saveEq();          // snapshot every channel's EQ into the settings store

    void buildBassCard(QVBoxLayout* root);
    void reflectBass();
    void showCrossoverHint(float crossoverHz);   // model -> bass controls
    void pushBass();      // bass model -> real audio
    void saveBass();      // snapshot every channel's bass stage
    void restoreBass();   // load it back

    void rebuildPresetCombo();
    void onPresetSelected();
    void onSavePreset();
    void onImportPreset();
    void onExportPreset();

    std::array<dsp::EqSettings, sonar::audio::kAllChannels.size()> eq_;
    // Each channel's harmonic bass stage (a game wants its lows rebuilt, a voice chat does not).
    std::array<dsp::BassStageSettings, sonar::audio::kAllChannels.size()> bass_;
    int channel_ = 0;  // index into audio::kAllChannels, not into the buttons
    // Button id -> kAllChannels index. The page shows output channels only, so
    // the two stopped being the same thing once the microphone left.
    std::vector<int> buttonChannel_;
    audio::IEqualizerController* controller_ = nullptr;
    audio::IAudioDevices* devices_ = nullptr;
    class QSlider* gain_ = nullptr;
    QCheckBox* autoGain_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QComboBox* output_ = nullptr;
    // Last device-list revision drawn, so a device plugged in while the page is open shows up.
    std::uint64_t devicesRevision_ = ~0ULL;
    QString currentOutput_;  // node.name, empty = follow the default
    config::SettingsStore* settings_ = nullptr;

    EqCurve* curve_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QButtonGroup* channelGroup_ = nullptr;
    QButtonGroup* bandGroup_ = nullptr;

    QCheckBox* bassOn_ = nullptr;
    QSlider* bassCrossover_ = nullptr;
    QLabel* bassCrossoverHint_ = nullptr;
    QSlider* bassAmount_ = nullptr;
    QSlider* bassHarmonics_ = nullptr;
    QSlider* bassEvenWeight_ = nullptr;
    QSlider* bassAttack_ = nullptr;
    QSlider* bassRelease_ = nullptr;
    QCheckBox* bassReplaceLows_ = nullptr;
    QCheckBox* bassLimiter_ = nullptr;

    QTimer* eqFlush_ = nullptr;  // throttles audio updates while dragging
    bool eqDirty_ = false;
    bool bassDirty_ = false;
    // Set while reflectBass() writes the controls, so the handlers it trips do
    // not read half-updated widgets back into the model.
    bool bassSyncing_ = false;
};

}  // namespace sonar::ui
