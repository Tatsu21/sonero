#pragma once

#include <array>

#include <QWidget>

#include "audio/Channel.h"
#include "dsp/Equalizer.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QTimer;

namespace sonar::audio {
class IEqualizerController;
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
                           config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

private:
    [[nodiscard]] dsp::EqSettings& current();
    void reflectControls(bool includeCombo);  // model -> controls
    void loadChannel();
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
