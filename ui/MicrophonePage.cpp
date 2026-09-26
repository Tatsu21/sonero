#include "ui/MicrophonePage.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/Channel.h"
#include "audio/IChannelController.h"
#include "audio/IEqualizerController.h"
#include "audio/IMicrophoneController.h"
#include "config/SettingsStore.h"
#include "ui/widgets/EqCurve.h"

namespace sonar::ui {

using audio::ChannelId;

namespace {
constexpr ChannelId kMic = ChannelId::Microphone;

QLabel* caption(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("CardKey"));
    label->setMinimumWidth(120);
    return label;
}

// A titled card; returns the body layout to fill.
QVBoxLayout* makeCard(QVBoxLayout* root, const QString& title, const QString& subtitle,
                      bool soon) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 18, 22, 20);
    body->setSpacing(8);

    auto* headRow = new QHBoxLayout;
    auto* t = new QLabel(title);
    t->setObjectName(QStringLiteral("SectionTitle"));
    headRow->addWidget(t);
    headRow->addStretch(1);
    if (soon) {
        auto* badge = new QLabel(QStringLiteral("PREPARED"));
        badge->setObjectName(QStringLiteral("SoonBadge"));
        headRow->addWidget(badge);
    }
    body->addLayout(headRow);

    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setObjectName(QStringLiteral("Hint"));
        s->setWordWrap(true);
        body->addWidget(s);
    }
    body->addSpacing(4);
    root->addWidget(frame);
    return body;
}

// A caption + horizontal slider + value label row.
QSlider* sliderRow(QVBoxLayout* body, const QString& label, int min, int max, int value,
                   const QString& suffix, bool enabled = true) {
    auto* row = new QHBoxLayout;
    auto* key = caption(label);
    row->addWidget(key);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setEnabled(enabled);
    auto* val = new QLabel(QStringLiteral("%1%2").arg(value).arg(suffix));
    val->setObjectName(QStringLiteral("VolumeValue"));
    val->setMinimumWidth(52);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // The caption and the readout are part of the control: a live-looking value
    // beside a dead slider is the one thing that would still read as usable.
    key->setEnabled(enabled);
    val->setEnabled(enabled);
    // The readout lives on this signal, so wrapping the slider in a
    // QSignalBlocker to set it silently desynchronises the two: the handle moves
    // and the number does not. Use showMicSettings(), which guards the handlers
    // with syncing_ instead of silencing the slider.
    QObject::connect(slider, &QSlider::valueChanged, val,
                     [val, suffix](int v) { val->setText(QStringLiteral("%1%2").arg(v).arg(suffix)); });
    row->addWidget(slider, 1);
    row->addWidget(val);
    body->addLayout(row);
    return slider;
}

// A checkbox for a card that is only prepared: shown, explained, not clickable.
QCheckBox* preparedCheck(const QString& text) {
    auto* box = new QCheckBox(text);
    box->setEnabled(false);
    return box;
}
}  // namespace

MicrophonePage::MicrophonePage(audio::IChannelController* controller,
                               audio::IEqualizerController* eqController,
                               audio::IMicrophoneController* micController,
                               config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent),
      controller_(controller),
      eqController_(eqController),
      micController_(micController),
      settings_(settings) {
    // Start from whatever the chain is already running, so the page reflects the
    // audio rather than resetting it on every visit.
    mic_ = micController_ != nullptr ? micController_->micSettings()
                                     : dsp::presetSettings(dsp::MicPreset::Podcast);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* page = new QWidget;
    scroll->setWidget(page);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("Microphone"), page);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("Your mic is a virtual source apps can select. Level and the voice "
                       "equalizer are live; the processing chain is on its way"), page);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    root->addLayout(head);

    // --- Chain preset ---
    // First, because it is the control that answers "what is this doing to my
    // voice" fastest: Raw switches every stage off, which is the A/B that makes
    // the rest of the page audible.
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(caption(QStringLiteral("Chain preset")));
    chainPreset_ = new QComboBox;
    for (const dsp::MicPreset preset :
         {dsp::MicPreset::Raw, dsp::MicPreset::Podcast, dsp::MicPreset::Streaming,
          dsp::MicPreset::Meeting}) {
        const std::string_view name = dsp::micPresetName(preset);
        chainPreset_->addItem(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())),
                              static_cast<int>(preset));
    }
    chainPreset_->setCurrentIndex(1);  // Podcast, which is what the chain starts on
    // Sized to its contents rather than stretched: a four-item list spread across
    // the whole window reads as a text field, and the room belongs to the
    // sentence explaining it.
    chainPreset_->setMinimumWidth(180);
    presetRow->addWidget(chainPreset_);
    auto* presetHint = new QLabel(
        QStringLiteral("Raw = every stage off, the microphone as the device delivers it."));
    presetHint->setObjectName(QStringLiteral("Hint"));
    presetHint->setWordWrap(true);
    presetRow->addWidget(presetHint, 1);
    root->addLayout(presetRow);

    // --- Input (live) ---
    QVBoxLayout* input = makeCard(root, QStringLiteral("Input"),
                                  QStringLiteral("Gain, mute and level of the virtual mic."),
                                  false);

    auto* deviceRow = new QHBoxLayout;
    deviceRow->addWidget(caption(QStringLiteral("Device")));
    // A label rather than a disabled combo box: the microphone follows the
    // system default, and a dropdown that cannot be dropped down is a promise
    // the page does not keep. Choosing a device arrives with the input node.
    auto* device = new QLabel(QStringLiteral("System default microphone"));
    device->setObjectName(QStringLiteral("Hint"));
    deviceRow->addWidget(device, 1);
    input->addLayout(deviceRow);

    // Decibels, not per cent: this is a trim on the way into the chain, and the
    // gate and the compressor below it have thresholds in decibels. A per-cent
    // fader would leave the user converting in their head.
    gain_ = sliderRow(input, QStringLiteral("Input gain"), -12, 24,
                      static_cast<int>(mic_.inputGainDb), QStringLiteral(" dB"), true);

    // Two gains, because they do different things and conflating them is why the
    // input fader felt broken. Input gain decides how hard the chain is driven —
    // raise it and the compressor simply works harder, so the result barely gets
    // louder, which is exactly what a compressor is for. Output level sits after
    // the compressor and before the limiter, and is the one that changes how loud
    // the other end hears you.
    outputGain_ = sliderRow(input, QStringLiteral("Output level"), -12, 12,
                            static_cast<int>(mic_.outputGainDb), QStringLiteral(" dB"), true);


    gainValue_ = nullptr;  // handled inside sliderRow

    auto* levelRow = new QHBoxLayout;
    levelRow->addWidget(caption(QStringLiteral("Input level")));
    level_ = new QProgressBar;
    level_->setObjectName(QStringLiteral("MicLevel"));
    level_->setRange(0, 100);
    level_->setValue(0);
    level_->setTextVisible(false);
    levelRow->addWidget(level_, 1);
    input->addLayout(levelRow);

    auto* muteRow = new QHBoxLayout;
    mute_ = new QCheckBox(QStringLiteral("Mute microphone"));
    muteRow->addWidget(mute_);
    muteRow->addStretch(1);
    input->addLayout(muteRow);

    // Below the controls it describes, not between two of them: a paragraph in
    // the middle of a column of key/value rows breaks the rhythm that makes them
    // scannable.
    auto* gainHint = new QLabel(
        QStringLiteral("Input gain sets how hard the processing is driven; the compressor "
                       "evens out whatever you feed it, so this changes the character more "
                       "than the volume. Output level is the loudness applications hear."));
    gainHint->setObjectName(QStringLiteral("Hint"));
    gainHint->setWordWrap(true);
    input->addSpacing(4);
    input->addWidget(gainHint);

    buildEqualizerCard(root);

    // --- Noise suppression (prepared) ---
    buildNoiseCard(root);

    // --- Noise gate (prepared) ---
    QVBoxLayout* gate = makeCard(
        root, QStringLiteral("Noise Gate"),
        QStringLiteral("Fades the microphone down between sentences instead of cutting it "
                       "off. Written and tested (dsp/MicChain), waiting on the same node."),
        true);
    gate->addWidget(preparedCheck(QStringLiteral("Enable noise gate")));
    sliderRow(gate, QStringLiteral("Threshold"), -80, 0, -45, QStringLiteral(" dB"), false);
    sliderRow(gate, QStringLiteral("Attack"), 0, 200, 10, QStringLiteral(" ms"), false);
    sliderRow(gate, QStringLiteral("Release"), 0, 1000, 150, QStringLiteral(" ms"), false);

    // --- Monitoring (prepared) ---
    QVBoxLayout* mon = makeCard(
        root, QStringLiteral("Monitoring"),
        QStringLiteral("Hear your own microphone in your headphones."), true);
    mon->addWidget(preparedCheck(QStringLiteral("Hear myself")));
    sliderRow(mon, QStringLiteral("Monitor level"), 0, 100, 50, QStringLiteral("%"), false);

    root->addStretch(1);

    // Restore what was persisted before wiring, so setting a control fires no
    // signal and nothing is written back the moment the page opens.
    if (settings_ != nullptr) {
        const QJsonObject m = settings_->section(QStringLiteral("microphone"));
        if (!m.isEmpty()) {
            // The preset first, because it sets the stages the controls below do
            // not cover — the gate, the de-esser, the compressor. Without it a
            // restart quietly put everyone back on Podcast, whatever they chose.
            const int storedPreset = m.value(QStringLiteral("preset"))
                                         .toInt(static_cast<int>(dsp::MicPreset::Podcast));
            const int index = chainPreset_->findData(storedPreset);
            if (index >= 0) {
                mic_ = dsp::presetSettings(static_cast<dsp::MicPreset>(storedPreset));
                const QSignalBlocker block(chainPreset_);
                chainPreset_->setCurrentIndex(index);
            }

            mic_.inputGainDb = static_cast<float>(
                std::clamp(m.value(QStringLiteral("inputGainDb")).toInt(0), -12, 24));
            mic_.outputGainDb = static_cast<float>(
                std::clamp(m.value(QStringLiteral("outputGainDb")).toInt(0), -12, 12));
            mic_.muted = m.value(QStringLiteral("muted")).toBool(false);
            mic_.noiseSuppression = m.value(QStringLiteral("noiseSuppression")).toBool(true);
        }
        showMicSettings();
    }

    connect(gain_, &QSlider::valueChanged, this, [this](int v) {
        if (syncing_) {
            return;
        }
        mic_.inputGainDb = static_cast<float>(v);
        pushMicSettings();
    });
    connect(outputGain_, &QSlider::valueChanged, this, [this](int v) {
        if (syncing_) {
            return;
        }
        mic_.outputGainDb = static_cast<float>(v);
        pushMicSettings();
    });
    connect(mute_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) {
            return;
        }
        mic_.muted = on;
        pushMicSettings();
    });
    connect(chainPreset_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncing_) {
            return;
        }
        const auto preset = static_cast<dsp::MicPreset>(chainPreset_->itemData(index).toInt());
        // The preset owns the processing, not the levels: someone who set their
        // trim and their output does not expect a preset to move them, and being
        // muted is a state, not a sound.
        const float in = mic_.inputGainDb;
        const float out = mic_.outputGainDb;
        const bool muted = mic_.muted;
        mic_ = dsp::presetSettings(preset);
        mic_.inputGainDb = in;
        mic_.outputGainDb = out;
        mic_.muted = muted;
        showMicSettings();
        pushMicSettings();
    });

    pushMicSettings();

    timer_ = new QTimer(this);
    timer_->setInterval(45);
    connect(timer_, &QTimer::timeout, this, &MicrophonePage::refresh);
    timer_->start();
}

void MicrophonePage::buildEqualizerCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Voice equalizer"),
        QStringLiteral("Shapes the microphone before anything else hears it — every "
                       "application recording from Sonero gets this curve, not just one."),
        false);

    // Ten bands rather than the thirty-one a channel gets. A voice is one source
    // in a narrow range, and thirty-one handles on a microphone is a way to make
    // it sound worse with more effort.
    dsp::resetBands(eq_, dsp::BandCount::Bands10);
    eq_.enabled = false;
    eq_.preset = dsp::EqPreset::Flat;

    if (settings_ != nullptr) {
        const QJsonObject stored = settings_->section(QStringLiteral("microphoneEq"));
        if (!stored.isEmpty()) {
            eq_ = config::eqFromJson(stored, eq_);
        }
    }

    auto* topRow = new QHBoxLayout;
    auto* enable = new QCheckBox(QStringLiteral("Enable"));
    enable->setChecked(eq_.enabled);
    topRow->addWidget(enable);
    topRow->addSpacing(12);
    topRow->addWidget(caption(QStringLiteral("Preset")));

    preset_ = new QComboBox;
    // Only the presets that mean something for a voice. A microphone has no use
    // for Bass Boost, and offering it is how someone ends up sounding muddy.
    for (const dsp::EqPreset preset :
         {dsp::EqPreset::Flat, dsp::EqPreset::Voice, dsp::EqPreset::Podcast,
          dsp::EqPreset::Warm, dsp::EqPreset::Bright, dsp::EqPreset::Custom}) {
        preset_->addItem(QString::fromUtf8(dsp::presetName(preset).data(),
                                           static_cast<qsizetype>(dsp::presetName(preset).size())),
                         static_cast<int>(preset));
    }
    preset_->setMinimumWidth(150);
    topRow->addWidget(preset_);
    topRow->addStretch(1);

    auto* reset = new QPushButton(QStringLiteral("Flat"));
    reset->setCursor(Qt::PointingHandCursor);
    topRow->addWidget(reset);
    body->addLayout(topRow);

    curve_ = new EqCurve;
    body->addWidget(curve_, 1);

    connect(enable, &QCheckBox::toggled, this, [this](bool on) {
        eq_.enabled = on;
        applyEq();
    });
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto preset = static_cast<dsp::EqPreset>(preset_->itemData(index).toInt());
        if (preset == dsp::EqPreset::Custom) {
            return;  // "Custom" describes what the user did, it does not set anything
        }
        dsp::applyPreset(eq_, preset);
        showEq();
        applyEq();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        dsp::applyPreset(eq_, dsp::EqPreset::Flat);
        showEq();
        applyEq();
    });
    // Dragging a band is what makes a curve the user's rather than a preset's, so
    // the preset name follows the curve instead of contradicting it.
    connect(curve_, &EqCurve::bandChanged, this, [this](int index, float gainDb) {
        if (index < 0 || index >= static_cast<int>(eq_.bands.size())) {
            return;
        }
        eq_.bands[static_cast<std::size_t>(index)].gainDb = gainDb;
        eq_.preset = dsp::EqPreset::Custom;
        showEq();
        applyEq();
    });

    showEq();
    applyEq();
}


void MicrophonePage::buildNoiseCard(QVBoxLayout* root) {
    const bool available =
        micController_ != nullptr && micController_->micNoiseSuppressionAvailable();

    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Noise suppression"),
        QStringLiteral("Removes keyboards, fans and the room with a trained model, "
                       "before the gate and the compressor ever see the signal — so "
                       "their thresholds sit against silence rather than against noise."),
        !available);

    noiseEnable_ = new QCheckBox(QStringLiteral("Enable noise suppression"));
    noiseEnable_->setChecked(available && mic_.noiseSuppression);
    noiseEnable_->setEnabled(available);
    connect(noiseEnable_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) {
            return;
        }
        mic_.noiseSuppression = on;
        pushMicSettings();
    });
    body->addWidget(noiseEnable_);

    // There is no "strength" slider, and that is deliberate: the model decides per
    // frequency band how much of each frame is voice. A percentage on top of it
    // would be a second guess layered over a trained one.
    noiseState_ = new QLabel;
    noiseState_->setObjectName(QStringLiteral("Hint"));
    noiseState_->setWordWrap(true);
    if (available) {
        noiseState_->setText(
            QStringLiteral("Model: %1 · adds 10 ms of latency while it is on.")
                .arg(QString::fromUtf8(micController_->micNoiseSuppressionBackend().data(),
                                       static_cast<qsizetype>(
                                           micController_->micNoiseSuppressionBackend().size()))));
    } else {
        // Say which package and why, rather than greying a switch out and leaving
        // the user to wonder whether their microphone is at fault.
        noiseState_->setText(
            QStringLiteral("Not in this build: RNNoise was not found when Sonero was "
                           "compiled. Install it (<code>rnnoise</code> on Arch, "
                           "<code>librnnoise-dev</code> on Debian) and rebuild, and this "
                           "switch turns on."));
        noiseState_->setTextFormat(Qt::RichText);
    }
    body->addWidget(noiseState_);

    auto* meterRow = new QHBoxLayout;
    meterRow->addWidget(caption(QStringLiteral("Removing")));
    noiseMeter_ = new QProgressBar;
    noiseMeter_->setObjectName(QStringLiteral("MicLevel"));
    noiseMeter_->setRange(0, 100);
    noiseMeter_->setValue(0);
    // The number goes in a label beside the bar, not inside it: the styled bar
    // draws its text hard against the left edge, while every other row on this
    // page puts its value in a right-aligned label. One of them had to give.
    noiseMeter_->setTextVisible(false);
    noiseMeter_->setEnabled(available);
    meterRow->addWidget(noiseMeter_, 1);

    noiseValue_ = new QLabel(QStringLiteral("0%"));
    noiseValue_->setObjectName(QStringLiteral("VolumeValue"));
    noiseValue_->setMinimumWidth(52);
    noiseValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    noiseValue_->setEnabled(available);
    meterRow->addWidget(noiseValue_);
    body->addLayout(meterRow);
}

void MicrophonePage::showMicSettings() {
    // The signals are left alone on purpose. Each slider's readout is kept in
    // step by a connection of its own, so silencing the slider would move it
    // without moving the number beside it; `syncing_` stops only the handlers
    // that would push these values straight back to the audio thread.
    syncing_ = true;
    if (gain_ != nullptr) {
        gain_->setValue(static_cast<int>(mic_.inputGainDb));
    }
    if (outputGain_ != nullptr) {
        outputGain_->setValue(static_cast<int>(mic_.outputGainDb));
    }
    if (mute_ != nullptr) {
        mute_->setChecked(mic_.muted);
    }
    if (noiseEnable_ != nullptr) {
        noiseEnable_->setChecked(noiseEnable_->isEnabled() && mic_.noiseSuppression);
    }
    syncing_ = false;
}

void MicrophonePage::pushMicSettings() {
    if (micController_ != nullptr) {
        micController_->setMicSettings(mic_);
    }
    saveMic();
}
void MicrophonePage::showEq() {
    if (curve_ != nullptr) {
        curve_->setSettings(eq_);
    }
    if (preset_ != nullptr) {
        const int index = preset_->findData(static_cast<int>(eq_.preset));
        if (index >= 0 && index != preset_->currentIndex()) {
            const QSignalBlocker block(preset_);
            preset_->setCurrentIndex(index);
        }
    }
}

void MicrophonePage::applyEq() {
    if (eqController_ != nullptr) {
        eqController_->applyEqualizer(kMic, eq_);
    }
    if (settings_ != nullptr) {
        settings_->putSection(QStringLiteral("microphoneEq"), config::eqToJson(eq_));
    }
}

void MicrophonePage::refresh() {
    if (micController_ == nullptr) {
        return;
    }
    // One reader, because reading clears the peak holds: a second poller
    // elsewhere would see half the transients and so would this one.
    const audio::MicLevels levels = micController_->micLevels();
    level_->setValue(std::clamp(static_cast<int>(levels.inputPeak * 140.0f), 0, 100));

    if (noiseMeter_ != nullptr) {
        // 0 dB of reduction reads as nothing removed, 24 dB as everything the
        // model is willing to take out. Beyond that the scale stops meaning
        // anything to a person watching a bar.
        const float removed = std::clamp(-levels.noiseReductionDb / 24.0f, 0.0f, 1.0f);
        const int percent = static_cast<int>(removed * 100.0f);
        noiseMeter_->setValue(percent);
        if (noiseValue_ != nullptr) {
            noiseValue_->setText(QStringLiteral("%1%").arg(percent));
        }
    }
}

void MicrophonePage::saveMic() {
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject m;
    m[QStringLiteral("inputGainDb")] = static_cast<int>(mic_.inputGainDb);
    m[QStringLiteral("outputGainDb")] = static_cast<int>(mic_.outputGainDb);
    m[QStringLiteral("muted")] = mic_.muted;
    m[QStringLiteral("noiseSuppression")] = mic_.noiseSuppression;
    if (chainPreset_ != nullptr) {
        m[QStringLiteral("preset")] = chainPreset_->currentData().toInt();
    }
    settings_->putSection(QStringLiteral("microphone"), m);
}

}  // namespace sonar::ui
