#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QWidget>

#include "audio/IMicrophoneController.h"
#include "dsp/Equalizer.h"
#include "dsp/MicChain.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QSlider;
class QTimer;
class QVBoxLayout;

namespace sonar::audio {
class IChannelController;
class IEqualizerController;
class IMicrophoneController;
}

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

class EqCurve;

// The microphone page: the virtual mic's input, every dsp/MicChain stage, and
// monitoring. Owns the persisted state; the tray's switches use its public slots.
class MicrophonePage : public QWidget {
    Q_OBJECT

public:
    explicit MicrophonePage(audio::IChannelController* controller,
                            audio::IEqualizerController* eqController = nullptr,
                            audio::IMicrophoneController* micController = nullptr,
                            config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

    [[nodiscard]] std::vector<audio::MicInputDevice> inputDevices() const;
    [[nodiscard]] QString selectedDevice() const { return device_; }  // empty: system default
    [[nodiscard]] bool isMuted() const { return mic_.muted; }

public slots:
    void selectDevice(const QString& nodeName);
    void setMuted(bool muted);

signals:
    // The device list, the selection or the mute changed; the tray redraws.
    void microphoneStateChanged();

protected:
    // The meters only have something to show while the chain runs, and nothing
    // else makes it run unless an application happens to be recording.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct SliderBinding {
        QSlider* slider = nullptr;
        std::function<float&(dsp::MicSettings&)> field;
        int divisor = 1;
    };
    struct CheckBinding {
        QCheckBox* box = nullptr;
        std::function<bool&(dsp::MicSettings&)> field;
        QWidget* panel = nullptr;  // the controls this switch governs
        bool panelWhenOff = false;  // they apply when it is off, not when it is on
    };
    struct StageMeter {
        QProgressBar* bar = nullptr;
        QLabel* value = nullptr;
        std::function<float(const audio::MicLevels&)> read;
    };

    QVBoxLayout* stageCard(QVBoxLayout* root, const QString& title, const QString& subtitle,
                           const QString& enableText, std::function<bool&(dsp::MicSettings&)> field);
    QCheckBox* stageCheck(QVBoxLayout* body, const QString& text,
                          std::function<bool&(dsp::MicSettings&)> field, QWidget* panel,
                          bool panelWhenOff);
    QSlider* stageSlider(QVBoxLayout* body, const QString& label, int min, int max,
                         const QString& suffix, std::function<float&(dsp::MicSettings&)> field,
                         int divisor = 1);
    void addReductionMeter(QVBoxLayout* body, std::function<float(const audio::MicLevels&)> read);
    void markCustom();  // a stage was edited: this is no longer the preset's sound
    void buildDeviceRow(QVBoxLayout* input);
    void buildStageCards(QVBoxLayout* root);
    void buildMonitorCard(QVBoxLayout* root);
    void buildEchoCard(QVBoxLayout* root);
    void showEchoState();
    void refreshDevices();  // redraw the device list and what it is doing
    void pushMonitor();
    void refresh();
    void saveMic();  // persist input gain + mute
    void buildEqualizerCard(QVBoxLayout* root);
    void buildNoiseCard(QVBoxLayout* root);
    void pushMicSettings();   // send the chain settings to the audio thread
    void showMicSettings();   // repaint the controls from mic_, without re-sending
    void applyEq();  // push the curve to the audio path and persist it
    void showEq();   // repaint the curve and the preset name from eq_

    audio::IChannelController* controller_ = nullptr;
    audio::IEqualizerController* eqController_ = nullptr;
    audio::IMicrophoneController* micController_ = nullptr;
    config::SettingsStore* settings_ = nullptr;
    QTimer* timer_ = nullptr;
    QProgressBar* level_ = nullptr;
    QProgressBar* outputMeter_ = nullptr;
    QComboBox* deviceBox_ = nullptr;
    QLabel* deviceStatus_ = nullptr;
    QString device_;       // node.name the user chose; empty follows the system default
    QString deviceLabel_;  // its description, to name it while it is unplugged
    std::uint64_t seenInputRevision_ = 0;
    std::vector<SliderBinding> sliders_;
    std::vector<CheckBinding> checks_;
    std::vector<StageMeter> meters_;
    QCheckBox* echoEnable_ = nullptr;
    QComboBox* echoSpeakers_ = nullptr;
    QString echoSpeakersName_;  // output the speakers are on; empty: the system default
    void refreshEchoSpeakers();
    void pushEcho();
    QLabel* echoState_ = nullptr;
    bool echoShownActive_ = false;
    QCheckBox* monitorEnable_ = nullptr;
    QSlider* monitorLevel_ = nullptr;
    QLabel* latency_ = nullptr;
    QSlider* gain_ = nullptr;
    QSlider* outputGain_ = nullptr;
    QCheckBox* mute_ = nullptr;

    dsp::EqSettings eq_;
    EqCurve* curve_ = nullptr;
    QComboBox* preset_ = nullptr;

    // The chain's settings as the page has them; the audio thread gets a copy.
    dsp::MicSettings mic_;
    // True while the page writes its own controls; handlers check it instead of a
    // QSignalBlocker, which would also freeze the slider readouts.
    bool syncing_ = false;
    QComboBox* chainPreset_ = nullptr;
    QCheckBox* noiseEnable_ = nullptr;
    QLabel* noiseState_ = nullptr;
    QProgressBar* noiseMeter_ = nullptr;
    QLabel* noiseValue_ = nullptr;
};

}  // namespace sonar::ui
