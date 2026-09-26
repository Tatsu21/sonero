#pragma once

#include <QWidget>

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

// Dedicated microphone page.
//
// Live: input gain, mute, the level meter, and the voice equalizer — all of them
// reaching the virtual microphone source through the same path the channel pages
// use. Still prepared, and labelled as such: the gate, compressor, de-esser and
// noise suppression, whose DSP exists and is tested (dsp/MicChain) but has no
// node to run in yet.
class MicrophonePage : public QWidget {
    Q_OBJECT

public:
    explicit MicrophonePage(audio::IChannelController* controller,
                            audio::IEqualizerController* eqController = nullptr,
                            audio::IMicrophoneController* micController = nullptr,
                            config::SettingsStore* settings = nullptr, QWidget* parent = nullptr);

private:
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
    QSlider* gain_ = nullptr;
    QSlider* outputGain_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QCheckBox* mute_ = nullptr;

    dsp::EqSettings eq_;
    EqCurve* curve_ = nullptr;
    QComboBox* preset_ = nullptr;

    // The chain's settings as the page has them; the audio thread gets a copy.
    dsp::MicSettings mic_;
    // True while the page is writing its own controls. The handlers below check
    // it instead of the controls being wrapped in QSignalBlocker: blocking a
    // slider also silences the connection that keeps its readout in step, which
    // is how a restored setting ended up shown as "0 dB" next to a slider
    // sitting at the other end of its travel.
    bool syncing_ = false;
    QComboBox* chainPreset_ = nullptr;
    QCheckBox* noiseEnable_ = nullptr;
    QLabel* noiseState_ = nullptr;
    QProgressBar* noiseMeter_ = nullptr;
};

}  // namespace sonar::ui
