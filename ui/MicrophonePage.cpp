#include "ui/MicrophonePage.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QHideEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
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
//
// `divisor` is for values a slider cannot hold directly: at 10 it steps in tenths,
// so a ratio of 2.5:1 is the slider position 25.
QSlider* sliderRow(QVBoxLayout* body, const QString& label, int min, int max, int value,
                   const QString& suffix, bool enabled = true, int divisor = 1) {
    const auto format = [suffix, divisor](int v) {
        const QString number = divisor == 1 ? QString::number(v)
                                            : QString::number(static_cast<double>(v) / divisor, 'f', 1);
        return number + suffix;
    };
    auto* row = new QHBoxLayout;
    auto* key = caption(label);
    row->addWidget(key);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(value);
    slider->setEnabled(enabled);
    auto* val = new QLabel(format(slider->value()));
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
                     [val, format](int v) { val->setText(format(v)); });
    row->addWidget(slider, 1);
    row->addWidget(val);
    body->addLayout(row);
    return slider;
}

// A group of controls a switch can grey out together. Transparent, because the
// application sheet paints plain widgets with the window colour, not the card's.
QWidget* makePanel(QVBoxLayout** layout) {
    auto* panel = new QWidget;
    panel->setObjectName(QStringLiteral("StagePanel"));
    panel->setStyleSheet(QStringLiteral("#StagePanel { background:transparent; }"));
    *layout = new QVBoxLayout(panel);
    (*layout)->setContentsMargins(0, 0, 0, 0);
    (*layout)->setSpacing(8);
    return panel;
}

// Bluetooth inputs get a warning in the list: choosing one changes how the
// headset sounds, not only where the voice comes from.
QString deviceText(const audio::MicInputDevice& device) {
    const QString text = QString::fromStdString(device.description);
    return device.type == audio::DeviceType::Bluetooth
               ? QStringLiteral("%1 (Bluetooth)").arg(text)
               : text;
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
        QStringLiteral("Pick your microphone here, then select \"Sonero Microphone\" in your "
                       "apps. Every app recording from it hears this chain."), page);
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
          dsp::MicPreset::Meeting, dsp::MicPreset::Custom}) {
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
        QStringLiteral("Raw = every stage off, the microphone as the device delivers it. "
                       "Changing any stage below makes it Custom."));
    presetHint->setObjectName(QStringLiteral("Hint"));
    presetHint->setWordWrap(true);
    presetRow->addWidget(presetHint, 1);
    root->addLayout(presetRow);

    // --- Input (live) ---
    QVBoxLayout* input = makeCard(root, QStringLiteral("Input"),
                                  QStringLiteral("The device, its gain, mute and level."),
                                  false);
    buildDeviceRow(input);

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

    // What leaves the chain, because the input meter stands before every stage and
    // so cannot show what the gain, the gate or the mute did.
    auto* outputRow = new QHBoxLayout;
    outputRow->addWidget(caption(QStringLiteral("Output")));
    outputMeter_ = new QProgressBar;
    outputMeter_->setObjectName(QStringLiteral("MicLevel"));
    outputMeter_->setRange(0, 100);
    outputMeter_->setValue(0);
    outputMeter_->setTextVisible(false);
    outputRow->addWidget(outputMeter_, 1);
    input->addLayout(outputRow);

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

    buildEchoCard(root);
    buildEqualizerCard(root);

    // In the order the signal meets them, so the page reads as the chain does.
    buildNoiseCard(root);
    buildStageCards(root);
    buildMonitorCard(root);

    root->addStretch(1);

    // Restore what was persisted before wiring, so setting a control fires no
    // signal and nothing is written back the moment the page opens.
    if (settings_ != nullptr) {
        const QJsonObject m = settings_->section(QStringLiteral("microphone"));
        if (!m.isEmpty()) {
            // The preset first, because it decides the stages. They are only read
            // back for Custom: a named preset means whatever it means in this
            // version, not what it happened to mean when it was last saved.
            const int storedPreset = m.value(QStringLiteral("preset"))
                                         .toInt(static_cast<int>(dsp::MicPreset::Podcast));
            const int index = chainPreset_->findData(storedPreset);
            if (index >= 0) {
                mic_ = dsp::presetSettings(static_cast<dsp::MicPreset>(storedPreset));
                const QSignalBlocker block(chainPreset_);
                chainPreset_->setCurrentIndex(index);
            }

            if (storedPreset == static_cast<int>(dsp::MicPreset::Custom)) {
                mic_ = config::micFromJson(m, mic_);
            } else {
                const dsp::MicSettings stored = config::micFromJson(m, mic_);
                mic_.inputGainDb = stored.inputGainDb;
                mic_.outputGainDb = stored.outputGainDb;
                mic_.muted = stored.muted;
                mic_.noiseSuppression = stored.noiseSuppression;
            }

            device_ = m.value(QStringLiteral("device")).toString();
            if (echoEnable_ != nullptr) {
                const QSignalBlocker blockEcho(echoEnable_);
                echoEnable_->setChecked(m.value(QStringLiteral("echoCancel")).toBool(false));
                echoSpeakersName_ = m.value(QStringLiteral("echoSpeakers")).toString();
            }
            deviceLabel_ = m.value(QStringLiteral("deviceLabel")).toString();
            if (monitorEnable_ != nullptr) {
                syncing_ = true;
                monitorEnable_->setChecked(m.value(QStringLiteral("monitor")).toBool(false));
                monitorLevel_->setValue(
                    std::clamp(m.value(QStringLiteral("monitorLevel")).toInt(50), 0, 100));
                syncing_ = false;
            }
        }
    }
    showMicSettings();

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
        setMuted(on);
    });
    connect(chainPreset_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncing_) {
            return;
        }
        const auto preset = static_cast<dsp::MicPreset>(chainPreset_->itemData(index).toInt());
        if (preset == dsp::MicPreset::Custom) {
            saveMic();  // "Custom" describes what the user did, it does not set anything
            return;
        }
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
    if (micController_ != nullptr && !device_.isEmpty()) {
        micController_->setMicCaptureDevice(device_.toStdString());
    }
    pushMonitor();
    if (echoEnable_->isChecked()) {
        pushEcho();
    }
    refreshEchoSpeakers();
    showEchoState();
    connect(echoSpeakers_, &QComboBox::activated, this, [this](int index) {
        echoSpeakersName_ = echoSpeakers_->itemData(index).toString();
        pushEcho();
        saveMic();
        showEchoState();
    });
    refreshDevices();
    connect(deviceBox_, &QComboBox::activated, this,
            [this](int index) { selectDevice(deviceBox_->itemData(index).toString()); });

    timer_ = new QTimer(this);
    timer_->setInterval(45);
    connect(timer_, &QTimer::timeout, this, &MicrophonePage::refresh);
    timer_->start();
}

void MicrophonePage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (micController_ != nullptr) {
        micController_->setMicLive(true);
    }
}

void MicrophonePage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (micController_ != nullptr) {
        micController_->setMicLive(false);
    }
}

std::vector<audio::MicInputDevice> MicrophonePage::inputDevices() const {
    return micController_ != nullptr ? micController_->micInputDevices()
                                     : std::vector<audio::MicInputDevice>{};
}

void MicrophonePage::selectDevice(const QString& nodeName) {
    device_ = nodeName;
    deviceLabel_.clear();
    for (const audio::MicInputDevice& device : inputDevices()) {
        if (QString::fromStdString(device.name) == nodeName) {
            deviceLabel_ = QString::fromStdString(device.description);
        }
    }
    if (micController_ != nullptr) {
        micController_->setMicCaptureDevice(device_.toStdString());
    }
    saveMic();
    refreshDevices();
}

void MicrophonePage::setMuted(bool muted) {
    mic_.muted = muted;
    showMicSettings();
    pushMicSettings();
    emit microphoneStateChanged();
}

void MicrophonePage::buildDeviceRow(QVBoxLayout* input) {
    auto* row = new QHBoxLayout;
    row->addWidget(caption(QStringLiteral("Device")));
    deviceBox_ = new QComboBox;
    row->addWidget(deviceBox_, 1);
    input->addLayout(row);

    deviceStatus_ = new QLabel;
    deviceStatus_->setObjectName(QStringLiteral("Hint"));
    deviceStatus_->setWordWrap(true);
    deviceStatus_->hide();
    input->addWidget(deviceStatus_);
}

void MicrophonePage::refreshDevices() {
    if (deviceBox_ == nullptr) {
        return;
    }
    const std::vector<audio::MicInputDevice> devices = inputDevices();
    const auto describe = [&devices](const std::string& name) {
        for (const audio::MicInputDevice& device : devices) {
            if (device.name == name) {
                return QString::fromStdString(device.description);
            }
        }
        return QString::fromStdString(name);
    };

    const std::string defaultInput =
        micController_ != nullptr ? micController_->micDefaultInput() : std::string();
    const std::string active =
        micController_ != nullptr ? micController_->micActiveCaptureDevice() : std::string();

    const QSignalBlocker block(deviceBox_);
    deviceBox_->clear();
    deviceBox_->addItem(
        defaultInput.empty()
            ? QStringLiteral("System default")
            : QStringLiteral("System default (%1)").arg(describe(defaultInput)),
        QString());
    bool chosenConnected = device_.isEmpty();
    for (const audio::MicInputDevice& device : devices) {
        deviceBox_->addItem(deviceText(device), QString::fromStdString(device.name));
        if (QString::fromStdString(device.name) == device_) {
            chosenConnected = true;
        }
    }
    // Unplugged, but still the choice: dropping it from the list would silently
    // turn "my USB mic" into "whatever is the default" the next time it is saved.
    if (!chosenConnected) {
        deviceBox_->addItem(QStringLiteral("%1 (not connected)")
                                .arg(deviceLabel_.isEmpty() ? device_ : deviceLabel_),
                            device_);
    }
    deviceBox_->setCurrentIndex(std::max(0, deviceBox_->findData(device_)));

    QString status;
    if (!chosenConnected) {
        status = active.empty()
                     ? QStringLiteral("Not connected. Sonero will use it again when it returns.")
                     : QStringLiteral("Not connected. Using %1 until it returns.")
                           .arg(describe(active));
    } else if (active.empty()) {
        status = defaultInput.rfind("bluez", 0) == 0
                     ? QStringLiteral("The system default is a Bluetooth headset, which Sonero "
                                      "leaves alone so your music keeps its quality. Choose it "
                                      "above to use its microphone anyway.")
                     : QStringLiteral("No microphone is available.");
    } else if (active.rfind("bluez", 0) == 0) {
        status = QStringLiteral("Using a Bluetooth microphone switches the headset to its call "
                                "profile, and playback through it drops to call quality.");
    }
    deviceStatus_->setText(status);
    deviceStatus_->setVisible(!status.isEmpty());

    emit microphoneStateChanged();
}

QVBoxLayout* MicrophonePage::stageCard(QVBoxLayout* root, const QString& title,
                                       const QString& subtitle, const QString& enableText,
                                       std::function<bool&(dsp::MicSettings&)> field) {
    QVBoxLayout* body = makeCard(root, title, subtitle, false);
    QVBoxLayout* panelLayout = nullptr;
    QWidget* panel = makePanel(&panelLayout);
    stageCheck(body, enableText, std::move(field), panel, false);
    body->addWidget(panel);
    return panelLayout;
}

QCheckBox* MicrophonePage::stageCheck(QVBoxLayout* body, const QString& text,
                                      std::function<bool&(dsp::MicSettings&)> field, QWidget* panel,
                                      bool panelWhenOff) {
    auto* box = new QCheckBox(text);
    body->addWidget(box);
    checks_.push_back({box, field, panel, panelWhenOff});
    // Greying out runs even while syncing_; pushing back to the audio thread does not.
    connect(box, &QCheckBox::toggled, panel,
            [panel, panelWhenOff](bool on) { panel->setEnabled(on != panelWhenOff); });
    connect(box, &QCheckBox::toggled, this, [this, field](bool on) {
        if (syncing_) {
            return;
        }
        field(mic_) = on;
        markCustom();
    });
    return box;
}

QSlider* MicrophonePage::stageSlider(QVBoxLayout* body, const QString& label, int min, int max,
                                     const QString& suffix,
                                     std::function<float&(dsp::MicSettings&)> field, int divisor) {
    const int value = static_cast<int>(std::lround(field(mic_) * static_cast<float>(divisor)));
    QSlider* slider = sliderRow(body, label, min, max, value, suffix, true, divisor);
    sliders_.push_back({slider, field, divisor});
    connect(slider, &QSlider::valueChanged, this, [this, field, divisor](int v) {
        if (syncing_) {
            return;
        }
        field(mic_) = static_cast<float>(v) / static_cast<float>(divisor);
        markCustom();
    });
    return slider;
}

void MicrophonePage::addReductionMeter(QVBoxLayout* body,
                                       std::function<float(const audio::MicLevels&)> read) {
    auto* row = new QHBoxLayout;
    row->addWidget(caption(QStringLiteral("Reduction")));
    auto* bar = new QProgressBar;
    bar->setObjectName(QStringLiteral("MicLevel"));
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    row->addWidget(bar, 1);
    auto* value = new QLabel(QStringLiteral("0 dB"));
    value->setObjectName(QStringLiteral("VolumeValue"));
    value->setMinimumWidth(52);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(value);
    body->addLayout(row);
    meters_.push_back({bar, value, std::move(read)});
}

void MicrophonePage::buildStageCards(QVBoxLayout* root) {
    const QString db = QStringLiteral(" dB");
    const QString ms = QStringLiteral(" ms");
    const QString hz = QStringLiteral(" Hz");
    const QString toOne = QStringLiteral(":1");

    QVBoxLayout* highPass = stageCard(
        root, QStringLiteral("High-pass filter"),
        QStringLiteral("Removes rumble and plosives below the voice, before they can drive "
                       "every stage after it."),
        QStringLiteral("Enable high-pass filter"),
        [](dsp::MicSettings& s) -> bool& { return s.highPass.enabled; });
    stageSlider(highPass, QStringLiteral("Frequency"), 20, 300, hz,
                [](dsp::MicSettings& s) -> float& { return s.highPass.frequencyHz; });

    QVBoxLayout* gate = stageCard(
        root, QStringLiteral("Noise gate"),
        QStringLiteral("Fades the microphone down between sentences instead of cutting it off. "
                       "Set the threshold above your room and below your voice."),
        QStringLiteral("Enable noise gate"),
        [](dsp::MicSettings& s) -> bool& { return s.gate.enabled; });
    stageSlider(gate, QStringLiteral("Threshold"), -80, 0, db,
                [](dsp::MicSettings& s) -> float& { return s.gate.thresholdDb; });
    stageSlider(gate, QStringLiteral("Ratio"), 10, 100, toOne,
                [](dsp::MicSettings& s) -> float& { return s.gate.ratio; }, 10);
    stageSlider(gate, QStringLiteral("Attack"), 0, 50, ms,
                [](dsp::MicSettings& s) -> float& { return s.gate.attackMs; });
    stageSlider(gate, QStringLiteral("Hold"), 0, 500, ms,
                [](dsp::MicSettings& s) -> float& { return s.gate.holdMs; });
    stageSlider(gate, QStringLiteral("Release"), 10, 1000, ms,
                [](dsp::MicSettings& s) -> float& { return s.gate.releaseMs; });
    addReductionMeter(gate, [](const audio::MicLevels& l) { return l.gateReductionDb; });

    QVBoxLayout* deEsser = stageCard(
        root, QStringLiteral("De-esser"),
        QStringLiteral("Ducks the voice for the few milliseconds an S lasts, instead of dulling "
                       "it permanently the way an equalizer cut would."),
        QStringLiteral("Enable de-esser"),
        [](dsp::MicSettings& s) -> bool& { return s.deEsser.enabled; });
    stageSlider(deEsser, QStringLiteral("Frequency"), 3000, 10000, hz,
                [](dsp::MicSettings& s) -> float& { return s.deEsser.frequencyHz; });
    stageSlider(deEsser, QStringLiteral("Threshold"), -60, 0, db,
                [](dsp::MicSettings& s) -> float& { return s.deEsser.thresholdDb; });
    stageSlider(deEsser, QStringLiteral("Ratio"), 10, 100, toOne,
                [](dsp::MicSettings& s) -> float& { return s.deEsser.ratio; }, 10);
    addReductionMeter(deEsser, [](const audio::MicLevels& l) { return l.deEsserReductionDb; });

    QVBoxLayout* comp = stageCard(
        root, QStringLiteral("Compressor"),
        QStringLiteral("Evens out the distance between your quietest and loudest words, so "
                       "leaning back does not lose you."),
        QStringLiteral("Enable compressor"),
        [](dsp::MicSettings& s) -> bool& { return s.compressor.enabled; });
    stageSlider(comp, QStringLiteral("Threshold"), -60, 0, db,
                [](dsp::MicSettings& s) -> float& { return s.compressor.thresholdDb; });
    stageSlider(comp, QStringLiteral("Ratio"), 10, 100, toOne,
                [](dsp::MicSettings& s) -> float& { return s.compressor.ratio; }, 10);
    stageSlider(comp, QStringLiteral("Knee"), 0, 12, db,
                [](dsp::MicSettings& s) -> float& { return s.compressor.kneeDb; });
    stageSlider(comp, QStringLiteral("Attack"), 0, 100, ms,
                [](dsp::MicSettings& s) -> float& { return s.compressor.attackMs; });
    stageSlider(comp, QStringLiteral("Release"), 10, 500, ms,
                [](dsp::MicSettings& s) -> float& { return s.compressor.releaseMs; });
    QVBoxLayout* makeupLayout = nullptr;
    QWidget* makeupPanel = makePanel(&makeupLayout);
    stageCheck(comp, QStringLiteral("Automatic makeup gain"),
               [](dsp::MicSettings& s) -> bool& { return s.compressor.autoMakeup; }, makeupPanel,
               true);
    stageSlider(makeupLayout, QStringLiteral("Makeup"), 0, 24, db,
                [](dsp::MicSettings& s) -> float& { return s.compressor.makeupDb; });
    comp->addWidget(makeupPanel);
    addReductionMeter(comp, [](const audio::MicLevels& l) { return l.compressorReductionDb; });

    QVBoxLayout* limiter = stageCard(
        root, QStringLiteral("Limiter"),
        QStringLiteral("The last stage: nothing leaves above the ceiling, whatever the gains "
                       "above it are doing. Adds 2 ms of latency."),
        QStringLiteral("Enable limiter"),
        [](dsp::MicSettings& s) -> bool& { return s.limiter.enabled; });
    stageSlider(limiter, QStringLiteral("Ceiling"), -12, 0, db,
                [](dsp::MicSettings& s) -> float& { return s.limiter.ceilingDb; });
    stageSlider(limiter, QStringLiteral("Release"), 10, 500, ms,
                [](dsp::MicSettings& s) -> float& { return s.limiter.releaseMs; });
    addReductionMeter(limiter, [](const audio::MicLevels& l) { return l.limiterReductionDb; });
}

void MicrophonePage::buildEchoCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Echo cancellation"),
        QStringLiteral("For speakers: whatever your default output plays — music, games, the "
                       "people in your call — is taken back out of the microphone, so the "
                       "other end hears you and not your desktop. Not needed with headphones."),
        false);

    // Not part of any preset: it depends on what is on your desk, not on how you
    // want your voice to sound.
    echoEnable_ = new QCheckBox(QStringLiteral("Remove desktop sound from the microphone"));
    body->addWidget(echoEnable_);

    // Which device, because Sonero's channels can each play somewhere else, and
    // only what plays on the one it listens to is removed.
    auto* speakersRow = new QHBoxLayout;
    speakersRow->addWidget(caption(QStringLiteral("Speakers")));
    echoSpeakers_ = new QComboBox;
    speakersRow->addWidget(echoSpeakers_, 1);
    body->addLayout(speakersRow);

    echoState_ = new QLabel;
    echoState_->setObjectName(QStringLiteral("Hint"));
    echoState_->setWordWrap(true);
    body->addWidget(echoState_);

    connect(echoEnable_, &QCheckBox::toggled, this, [this] {
        pushEcho();
        saveMic();
        showEchoState();
    });
}

void MicrophonePage::pushEcho() {
    if (micController_ != nullptr) {
        micController_->setMicEchoCancellation(echoEnable_->isChecked(),
                                               echoSpeakersName_.toStdString());
    }
}

void MicrophonePage::refreshEchoSpeakers() {
    if (echoSpeakers_ == nullptr || micController_ == nullptr) {
        return;
    }
    const std::vector<audio::AudioDevice> outputs = micController_->micEchoOutputs();
    const std::string defaultOutput = micController_->micDefaultOutput();
    QString defaultLabel;
    for (const audio::AudioDevice& output : outputs) {
        if (output.name == defaultOutput) {
            defaultLabel = QString::fromStdString(output.description);
        }
    }

    const QSignalBlocker block(echoSpeakers_);
    echoSpeakers_->clear();
    echoSpeakers_->addItem(defaultLabel.isEmpty()
                               ? QStringLiteral("System default output")
                               : QStringLiteral("System default output (%1)").arg(defaultLabel),
                           QString());
    for (const audio::AudioDevice& output : outputs) {
        echoSpeakers_->addItem(QString::fromStdString(output.description),
                               QString::fromStdString(output.name));
    }
    if (echoSpeakers_->findData(echoSpeakersName_) < 0) {
        echoSpeakers_->addItem(QStringLiteral("%1 (not connected)").arg(echoSpeakersName_),
                               echoSpeakersName_);
    }
    echoSpeakers_->setCurrentIndex(std::max(0, echoSpeakers_->findData(echoSpeakersName_)));
}

void MicrophonePage::showEchoState() {
    if (echoState_ == nullptr) {
        return;
    }
    const bool active = micController_ != nullptr && micController_->micEchoCancellationActive();
    echoShownActive_ = active;
    if (!echoEnable_->isChecked()) {
        echoState_->setText(QStringLiteral("Keep the speakers at a sensible volume: what is far "
                                           "louder than your voice cannot be fully removed."));
    } else if (active) {
        echoState_->setText(QStringLiteral(
            "On. Only what plays on the speakers above is removed — a channel sent to another "
            "output on the Channels page is not. It needs a few seconds of sound to learn your "
            "room; until then some echo can get through."));
    } else {
        echoState_->setText(QStringLiteral(
            "Not running: it needs a microphone, a default output, and PipeWire's echo-cancel "
            "module (part of libspa-0.2-modules / pipewire)."));
    }
}

void MicrophonePage::buildMonitorCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Monitoring"),
        QStringLiteral("Hear what your apps hear, on your default output. Use headphones: on "
                       "speakers the microphone picks itself up and howls."),
        false);

    // Off on every first run and never tied to a preset, for the same reason.
    monitorEnable_ = new QCheckBox(QStringLiteral("Hear myself"));
    body->addWidget(monitorEnable_);
    monitorLevel_ = sliderRow(body, QStringLiteral("Monitor level"), 0, 100, 50,
                              QStringLiteral("%"));

    latency_ = new QLabel;
    latency_->setObjectName(QStringLiteral("Hint"));
    body->addWidget(latency_);

    const auto changed = [this] {
        if (syncing_) {
            return;
        }
        pushMonitor();
        saveMic();
    };
    connect(monitorEnable_, &QCheckBox::toggled, this, changed);
    connect(monitorLevel_, &QSlider::valueChanged, this, changed);
}

void MicrophonePage::pushMonitor() {
    if (micController_ != nullptr && monitorEnable_ != nullptr) {
        micController_->setMicMonitor(monitorEnable_->isChecked(),
                                      static_cast<float>(monitorLevel_->value()) / 100.0f);
    }
}

void MicrophonePage::markCustom() {
    if (chainPreset_ != nullptr) {
        const int index = chainPreset_->findData(static_cast<int>(dsp::MicPreset::Custom));
        const QSignalBlocker block(chainPreset_);
        chainPreset_->setCurrentIndex(index);
    }
    pushMicSettings();
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
        markCustom();
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
        // Say exactly how to get it, rather than greying a switch out and leaving
        // the user to wonder whether their microphone is at fault. Naming a
        // package is not enough: only Arch ships one, so on nearly every system
        // the answer is the script this project carries for that reason.
        noiseState_->setText(QStringLiteral(
            "Not in this build: RNNoise was not found when Sonero was compiled, and "
            "almost no distribution packages it — Arch has <code>rnnoise</code>, "
            "Debian only in sid and forky, Ubuntu and Fedora not at all. To turn "
            "this on, build it once and rebuild Sonero:<br>"
            "<code>sudo apt install autoconf automake libtool</code><br>"
            "<code>sudo ./packaging/build-rnnoise.sh</code><br>"
            "<code>cmake -S . -B build &amp;&amp; cmake --build build -j</code>"));
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
    for (const SliderBinding& binding : sliders_) {
        binding.slider->setValue(static_cast<int>(
            std::lround(binding.field(mic_) * static_cast<float>(binding.divisor))));
    }
    for (const CheckBinding& binding : checks_) {
        const bool on = binding.field(mic_);
        binding.box->setChecked(on);
        // setChecked is silent when nothing changed, which the first paint relies on.
        binding.panel->setEnabled(on != binding.panelWhenOff);
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
    outputMeter_->setValue(std::clamp(static_cast<int>(levels.outputPeak * 140.0f), 0, 100));

    for (const StageMeter& meter : meters_) {
        const float reduction = std::max(0.0f, -meter.read(levels));
        // Same scale as the noise meter below.
        meter.bar->setValue(std::clamp(static_cast<int>(reduction / 24.0f * 100.0f), 0, 100));
        meter.value->setText(QStringLiteral("%1 dB").arg(static_cast<int>(std::lround(-reduction))));
    }

    if (latency_ != nullptr) {
        latency_->setText(QStringLiteral("The chain adds %1 ms of latency.")
                              .arg(static_cast<double>(micController_->micLatencyMs()), 0, 'f', 0));
    }

    if (const std::uint64_t revision = micController_->micInputRevision();
        revision != seenInputRevision_) {
        seenInputRevision_ = revision;
        refreshDevices();
        refreshEchoSpeakers();
    }
    if (micController_->micEchoCancellationActive() != echoShownActive_) {
        showEchoState();
    }

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
    QJsonObject m = config::micToJson(mic_);
    m[QStringLiteral("device")] = device_;
    m[QStringLiteral("deviceLabel")] = deviceLabel_;
    if (echoEnable_ != nullptr) {
        m[QStringLiteral("echoCancel")] = echoEnable_->isChecked();
        m[QStringLiteral("echoSpeakers")] = echoSpeakersName_;
    }
    if (monitorEnable_ != nullptr) {
        m[QStringLiteral("monitor")] = monitorEnable_->isChecked();
        m[QStringLiteral("monitorLevel")] = monitorLevel_->value();
    }
    if (chainPreset_ != nullptr) {
        m[QStringLiteral("preset")] = chainPreset_->currentData().toInt();
    }
    settings_->putSection(QStringLiteral("microphone"), m);
}

}  // namespace sonar::ui
