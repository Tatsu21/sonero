#include "ui/EqualizerPage.h"

#include <cmath>
#include <functional>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/IAudioDevices.h"
#include "audio/IEqualizerController.h"
#include "config/SettingsStore.h"
#include "ui/EqPresetStore.h"
#include "ui/UiKit.h"
#include "ui/widgets/EqCurve.h"

namespace sonar::ui {

using audio::ChannelId;

namespace {
QFrame* makeSegmented(const QStringList& labels, QButtonGroup*& group, QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("Segmented"));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(3);

    group = new QButtonGroup(frame);
    group->setExclusive(true);
    for (int i = 0; i < labels.size(); ++i) {
        auto* button = new QPushButton(labels.at(i), frame);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        if (i == 0) {
            button->setChecked(true);
        }
        group->addButton(button, i);
        layout->addWidget(button);
    }
    return frame;
}

// The gain readout: whole dB for manual moves, one decimal while auto-gain settles.
QString formatGainDb(float gainDb, bool fractional) {
    const QString number = fractional ? QString::number(gainDb, 'f', 1)
                                      : QString::number(static_cast<int>(std::lround(gainDb)));
    return gainDb > 0.0f ? QStringLiteral("+%1 dB").arg(number)
                         : QStringLiteral("%1 dB").arg(number);
}

QLabel* caption(const QString& text) { return makeLabel(text, "CardKey"); }

int bandIndexOf(dsp::BandCount count) {
    return count == dsp::BandCount::Bands10 ? 0 : count == dsp::BandCount::Bands15 ? 1 : 2;
}

constexpr std::array<dsp::BandCount, 3> kBandCounts = {
    dsp::BandCount::Bands10, dsp::BandCount::Bands15, dsp::BandCount::Bands31};

constexpr std::array<dsp::EqPreset, 24> kBuiltinPresets = {
    dsp::EqPreset::Flat,
    // music genres
    dsp::EqPreset::Music,      dsp::EqPreset::Rock,       dsp::EqPreset::Pop,
    dsp::EqPreset::Electronic, dsp::EqPreset::Dance,      dsp::EqPreset::HipHop,
    dsp::EqPreset::RnB,        dsp::EqPreset::Jazz,       dsp::EqPreset::Acoustic,
    dsp::EqPreset::Metal,      dsp::EqPreset::Classical,
    // tone shaping
    dsp::EqPreset::BassBoost,  dsp::EqPreset::BassReducer, dsp::EqPreset::TrebleBoost,
    dsp::EqPreset::TrebleReducer, dsp::EqPreset::Loudness, dsp::EqPreset::Warm,
    dsp::EqPreset::Bright,
    // voice / media / gaming
    dsp::EqPreset::Voice,      dsp::EqPreset::Podcast,    dsp::EqPreset::Movie,
    dsp::EqPreset::FPS,
    dsp::EqPreset::Custom};
}  // namespace

EqualizerPage::EqualizerPage(audio::IEqualizerController* controller,
                             config::SettingsStore* settings, audio::IAudioDevices* devices,
                             QWidget* parent)
    : QWidget(parent), controller_(controller), devices_(devices), settings_(settings) {
    for (auto& s : eq_) {
        dsp::resetBands(s, dsp::BandCount::Bands10);
        s.enabled = true;
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(18);

    addPageHeader(root, QStringLiteral("Channels"),
                  QStringLiteral("Per-channel gain, output and equalizer"));

    // Output channels only: the microphone's equalizer lives on its own page.
    QStringList channelLabels;
    for (std::size_t i = 0; i < audio::kAllChannels.size(); ++i) {
        if (audio::kAllChannels[i] == ChannelId::Microphone) continue;
        channelLabels << channelKey(audio::kAllChannels[i]);
        buttonChannel_.push_back(static_cast<int>(i));
    }
    root->addWidget(makeSegmented(channelLabels, channelGroup_, this));

    // --- Gain and output for the selected channel ---------------------------
    gain_ = new QSlider(Qt::Horizontal, this);
    gain_->setRange(-20, 6);
    gain_->setFixedWidth(150);
    gain_->setToolTip(QStringLiteral("Channel gain in dB (0 dB = unchanged)"));
    gainValue_ = makeLabel(QStringLiteral("0 dB"), "VolumeValue");
    gainValue_->setMinimumWidth(52);
    autoGain_ = new QCheckBox(QStringLiteral("Auto"), this);
    autoGain_->setToolTip(QStringLiteral(
        "Take as much gain as the material allows: it climbs to the top of the\n"
        "range wherever there is room, and comes back down by exactly what a peak\n"
        "overshoots. Slow going up, immediate coming down, so the level does not\n"
        "breathe. The slider follows along."));

    output_ = new QComboBox(this);
    output_->setMinimumWidth(190);
    output_->setToolTip(QStringLiteral("Which device this channel plays to"));

    auto* routeRow = new QHBoxLayout;
    routeRow->setSpacing(8);
    routeRow->addWidget(caption(QStringLiteral("Gain")));
    routeRow->addWidget(gain_);
    routeRow->addWidget(gainValue_);
    routeRow->addWidget(autoGain_);
    routeRow->addSpacing(18);
    routeRow->addWidget(caption(QStringLiteral("Output")));
    routeRow->addWidget(output_, 1);
    root->addLayout(routeRow);

    connect(gain_, &QSlider::valueChanged, this, [this](int db) {
        gainValue_->setText(formatGainDb(static_cast<float>(db), false));
        emit channelGainChanged(selectedChannel(),
                                static_cast<float>(db));
    });
    connect(autoGain_, &QCheckBox::toggled, this, [this](bool on) {
        gain_->setEnabled(!on);  // the loop owns the value while it runs
        emit channelAutoGainToggled(
            selectedChannel(), on);
    });
    connect(output_, &QComboBox::activated, this, [this](int) {
        currentOutput_ = output_->currentData().toString();
        emit channelOutputChanged(selectedChannel(),
                                  currentOutput_);
    });

    // Controls: enabled | presets + save/import/export | band count.
    enabledCheck_ = new QCheckBox(QStringLiteral("EQ enabled"), this);

    presetCombo_ = new QComboBox(this);
    presetCombo_->setMinimumWidth(150);

    auto* saveBtn = new QPushButton(QStringLiteral("Save…"), this);
    auto* importBtn = new QPushButton(QStringLiteral("Import…"), this);
    auto* exportBtn = new QPushButton(QStringLiteral("Export…"), this);
    for (QPushButton* b : {saveBtn, importBtn, exportBtn}) {
        b->setCursor(Qt::PointingHandCursor);
    }

    QButtonGroup* bandGroupLocal = nullptr;
    auto* bandSeg = makeSegmented({QStringLiteral("10"), QStringLiteral("15"), QStringLiteral("31")},
                                  bandGroupLocal, this);
    bandGroup_ = bandGroupLocal;

    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    controls->addWidget(enabledCheck_);
    controls->addStretch(1);
    controls->addWidget(caption(QStringLiteral("Preset")));
    controls->addWidget(presetCombo_);
    controls->addWidget(saveBtn);
    controls->addWidget(importBtn);
    controls->addWidget(exportBtn);
    controls->addSpacing(10);
    controls->addWidget(caption(QStringLiteral("Bands")));
    controls->addWidget(bandSeg);
    root->addLayout(controls);

    curve_ = new EqCurve(this);
    root->addWidget(curve_, 1);

    // Below the curve, because it works on what the curve produces: the bass
    // stage sits at the end of the same filter graph, after the biquads.
    buildBassCard(root);

    rebuildPresetCombo();

    connect(channelGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        if (id < 0 || id >= static_cast<int>(buttonChannel_.size())) return;
        channel_ = buttonChannel_[static_cast<std::size_t>(id)];
        loadChannel();
        emit channelSelected(selectedChannel());
    });
    connect(bandGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        dsp::EqSettings& s = current();
        const dsp::EqPreset keep = s.preset;
        dsp::resetBands(s, kBandCounts.at(static_cast<std::size_t>(id)));
        dsp::applyPreset(s, keep);  // re-apply so the shape survives a band change
        reflectControls(false);
        pushEq();
        saveEq();
    });
    connect(presetCombo_, &QComboBox::activated, this, [this](int) { onPresetSelected(); });
    connect(enabledCheck_, &QCheckBox::toggled, this, [this](bool on) {
        current().enabled = on;
        curve_->setSettings(current());
        pushEq();
        saveEq();
    });
    connect(saveBtn, &QPushButton::clicked, this, &EqualizerPage::onSavePreset);
    connect(importBtn, &QPushButton::clicked, this, &EqualizerPage::onImportPreset);
    connect(exportBtn, &QPushButton::clicked, this, &EqualizerPage::onExportPreset);
    connect(curve_, &EqCurve::bandChanged, this, &EqualizerPage::onBandChanged);

    // Flush drag edits to the audio path at a steady, low rate so the filters
    // don't click on every mouse move. Runs only while the page is shown.
    eqFlush_ = new QTimer(this);
    eqFlush_->setInterval(40);
    connect(eqFlush_, &QTimer::timeout, this, &EqualizerPage::flushPending);

    restoreEq();    // persisted per-channel EQ overrides the defaults above
    restoreBass();  // and the persisted harmonic bass stage of each channel

    loadChannel();
    // Apply every channel's (possibly restored) EQ to the audio path — not just
    // the visible one — so saved settings take effect immediately on launch.
    if (controller_ != nullptr) {
        for (std::size_t i = 0; i < eq_.size(); ++i) {
            // Not the microphone: its equalizer belongs to the microphone page.
            if (audio::kAllChannels[i] == ChannelId::Microphone) continue;
            controller_->applyEqualizer(audio::kAllChannels[i], eq_[i]);
            // The bass stage too, so a restored setting applies without selecting the channel.
            controller_->applyBassStage(audio::kAllChannels[i], bass_[i]);
        }
    }
}

void EqualizerPage::flushPending() {
    if (eqDirty_) {
        eqDirty_ = false;
        pushEq();
    }
    if (bassDirty_) {
        bassDirty_ = false;
        pushBass();
    }
    // Refresh the output list when a sink came or went.
    if (devices_ != nullptr && devices_->devicesRevision() != devicesRevision_) refreshOutputs();
}

void EqualizerPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    flushPending();  // catches a device that came or went while hidden
    eqFlush_->start();
}

void EqualizerPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    eqFlush_->stop();
    flushPending();  // an edit made just before leaving still reaches the audio
}

dsp::EqSettings& EqualizerPage::current() {
    return eq_[static_cast<std::size_t>(channel_)];
}

// --- Harmonic bass ----------------------------------------------------------

void EqualizerPage::buildBassCard(QVBoxLayout* root) {
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(18, 14, 18, 14);
    body->setSpacing(8);

    // Harmonic bass: synthesise the harmonics a small driver can reproduce instead of
    // boosting a fundamental it cannot.
    bassOn_ = new QCheckBox(QStringLiteral("Harmonic bass"), this);
    bassOn_->setToolTip(QStringLiteral(
        "Rebuild the low end the driver cannot reproduce, by synthesising the "
        "harmonics of the bass rather than asking for the fundamental."));
    bassReplaceLows_ = new QCheckBox(QStringLiteral("Replace lows"), this);
    bassReplaceLows_->setToolTip(QStringLiteral(
        "Filter the original below the crossover, so the driver stops being asked "
        "for what it cannot produce and the headroom goes to the harmonics that "
        "are audible. On anything with a real low end, leave this off."));
    bassLimiter_ = new QCheckBox(QStringLiteral("Limiter"), this);
    bassLimiter_->setToolTip(QStringLiteral(
        "Brick wall at -1 dBFS on this channel's output. Not a gain limit: it "
        "bounds what leaves the channel, never what the equalizer may do."));

    auto* head = new QHBoxLayout;
    head->setSpacing(10);
    head->addWidget(makeLabel(QStringLiteral("Harmonic bass"), "SectionTitle"));
    head->addSpacing(8);
    head->addWidget(bassOn_);
    head->addStretch(1);
    head->addWidget(bassReplaceLows_);
    head->addWidget(bassLimiter_);
    body->addLayout(head);

    bassCrossoverHint_ = makeLabel(QString(), "Hint", true);

    // Two columns: the three settings that shape the effect on the left, the
    // three that shape its timing and balance on the right.
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    const auto row = [this, grid](int r, int col, const QString& label, int lo, int hi,
                                  const std::function<QString(int)>& fmt,
                                  const QString& tip) {
        QLabel* key = caption(label);
        key->setMinimumWidth(78);
        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(lo, hi);
        QLabel* value = makeValueLabel(QString());
        value->setMinimumWidth(58);
        if (!tip.isEmpty()) {
            key->setToolTip(tip);
            slider->setToolTip(tip);
        }
        // The readout rides this signal, so reflectBass() must not silence the
        // slider to set it — it guards the handlers with bassSyncing_ instead.
        connect(slider, &QSlider::valueChanged, value,
                [value, fmt](int v) { value->setText(fmt(v)); });
        grid->addWidget(key, r, col * 3);
        grid->addWidget(slider, r, col * 3 + 1);
        grid->addWidget(value, r, col * 3 + 2);
        return slider;
    };

    bassCrossover_ = row(0, 0, QStringLiteral("Crossover"), 30, 300,
                         [](int v) { return QStringLiteral("%1 Hz").arg(v); },
                         QStringLiteral("Where this channel's transducer stops "
                                        "delivering. Below it the driver wastes "
                                        "excursion; above it the harmonics live.\n"
                                        "The setting that matters most."));
    bassAmount_ = row(1, 0, QStringLiteral("Amount"), -12, 24,
                      [](int v) {
                          return QStringLiteral("%1%2 dB")
                              .arg(v > 0 ? QStringLiteral("+") : QString())
                              .arg(v);
                      },
                      QStringLiteral("Level of the synthesised harmonics. Past about "
                                     "+12 dB they stop reading as the fundamental "
                                     "and start reading as their own pitch."));
    bassHarmonics_ = row(2, 0, QStringLiteral("Harmonics"), dsp::HarmonicBass::kMinHarmonics,
                         dsp::HarmonicBass::kMaxHarmonics,
                         [](int v) {
                             const char* suffix = v == 2 ? "nd" : (v == 3 ? "rd" : "th");
                             return QStringLiteral("up to %1%2").arg(v).arg(
                                 QLatin1String(suffix));
                         },
                         QStringLiteral("How many orders above the fundamental to "
                                        "generate. Fewer is cleaner and reads more as "
                                        "the fundamental; more is louder and buzzier."));
    bassEvenWeight_ = row(0, 1, QStringLiteral("Even / odd"), 0, 100,
                          [](int v) {
                              if (v == 0) return QStringLiteral("odd only");
                              if (v == 100) return QStringLiteral("even only");
                              return QStringLiteral("%1% even").arg(v);
                          },
                          QStringLiteral("Even orders read as fuller; odd orders read "
                                         "as growl and carry the pitch more strongly."));
    bassAttack_ = row(1, 1, QStringLiteral("Attack"), 1, 50,
                      [](int v) { return QStringLiteral("%1 ms").arg(v); },
                      QStringLiteral("How fast the harmonics follow the bass into a "
                                     "transient."));
    bassRelease_ = row(2, 1, QStringLiteral("Release"), 10, 400,
                       [](int v) { return QStringLiteral("%1 ms").arg(v); },
                       QStringLiteral("How long they hang on after it. Short is tight, "
                                      "long is smeared."));
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(4, 1);
    body->addLayout(grid);
    body->addWidget(bassCrossoverHint_);

    root->addWidget(frame);

    const auto onEdit = [this] {
        if (!bassSyncing_) {
            bassDirty_ = true;  // flushed by eqFlush_, like a curve drag
            saveBass();
        }
    };
    connect(bassOn_, &QCheckBox::toggled, this, onEdit);
    connect(bassReplaceLows_, &QCheckBox::toggled, this, onEdit);
    connect(bassLimiter_, &QCheckBox::toggled, this, onEdit);
    for (QSlider* slider : {bassCrossover_, bassAmount_, bassHarmonics_, bassEvenWeight_,
                            bassAttack_, bassRelease_}) {
        connect(slider, &QSlider::valueChanged, this, onEdit);
    }
}

void EqualizerPage::reflectBass() {
    if (bassOn_ == nullptr) return;
    const dsp::BassStageSettings& b = bass_[static_cast<std::size_t>(channel_)];
    bassSyncing_ = true;
    bassOn_->setChecked(b.bass.enabled);
    bassCrossover_->setValue(static_cast<int>(std::lround(b.bass.crossoverHz)));
    bassAmount_->setValue(static_cast<int>(std::lround(b.bass.amountDb)));
    bassHarmonics_->setValue(b.bass.harmonics);
    bassEvenWeight_->setValue(static_cast<int>(std::lround(b.bass.evenWeight * 100.0f)));
    bassAttack_->setValue(static_cast<int>(std::lround(b.bass.attackMs)));
    bassRelease_->setValue(static_cast<int>(std::lround(b.bass.releaseMs)));
    bassReplaceLows_->setChecked(b.bass.replaceLows);
    bassLimiter_->setChecked(b.limiterEnabled);
    bassSyncing_ = false;
    showCrossoverHint(b.bass.crossoverHz);
}

void EqualizerPage::showCrossoverHint(float crossoverHz) {
    bassCrossoverHint_->setText(QStringLiteral("Crossover suits: %1")
                                    .arg(toQString(dsp::harmonicBassCrossoverHint(crossoverHz))));
}

void EqualizerPage::pushBass() {
    if (bassOn_ == nullptr) return;
    dsp::BassStageSettings& b = bass_[static_cast<std::size_t>(channel_)];
    b.bass.enabled = bassOn_->isChecked();
    b.bass.crossoverHz = static_cast<float>(bassCrossover_->value());
    b.bass.amountDb = static_cast<float>(bassAmount_->value());
    b.bass.harmonics = bassHarmonics_->value();
    b.bass.evenWeight = static_cast<float>(bassEvenWeight_->value()) / 100.0f;
    b.bass.attackMs = static_cast<float>(bassAttack_->value());
    b.bass.releaseMs = static_cast<float>(bassRelease_->value());
    b.bass.replaceLows = bassReplaceLows_->isChecked();
    b.limiterEnabled = bassLimiter_->isChecked();

    showCrossoverHint(b.bass.crossoverHz);
    if (controller_ != nullptr) {
        controller_->applyBassStage(selectedChannel(), b);
    }
}

void EqualizerPage::saveBass() {
    if (settings_ == nullptr) return;
    QJsonObject channels;
    for (std::size_t i = 0; i < audio::kAllChannels.size(); ++i) {
        channels[channelKey(audio::kAllChannels[i])] = config::bassToJson(bass_[i]);
    }
    QJsonObject section;
    section[QStringLiteral("channels")] = channels;
    settings_->putSection(QStringLiteral("harmonicBass"), section);
}

void EqualizerPage::restoreBass() {
    if (settings_ == nullptr) return;
    const QJsonObject channels = settings_->section(QStringLiteral("harmonicBass"))
                                     .value(QStringLiteral("channels"))
                                     .toObject();
    for (std::size_t i = 0; i < audio::kAllChannels.size(); ++i) {
        const QJsonObject o = channels.value(channelKey(audio::kAllChannels[i])).toObject();
        if (!o.isEmpty()) {  // first run, or a channel never touched: keep the defaults
            bass_[i] = config::bassFromJson(o, bass_[i]);
        }
    }
}

void EqualizerPage::rebuildPresetCombo() {
    const QSignalBlocker block(presetCombo_);
    presetCombo_->clear();
    for (const dsp::EqPreset preset : kBuiltinPresets) {
        presetCombo_->addItem(toQString(dsp::presetName(preset)), static_cast<int>(preset));
    }
    const QStringList user = presets::userPresetNames();
    if (!user.isEmpty()) {
        presetCombo_->insertSeparator(presetCombo_->count());
        for (const QString& name : user) {
            presetCombo_->addItem(name, name);  // user items carry a QString
        }
    }
}

void EqualizerPage::onPresetSelected() {
    const QVariant chosen = presetCombo_->currentData();
    if (chosen.typeId() == QMetaType::QString) {
        if (const auto loaded = presets::load(chosen.toString())) {
            current() = *loaded;
        }
        reflectControls(false);  // keep the user preset shown in the combo
    } else {
        dsp::applyPreset(current(), static_cast<dsp::EqPreset>(chosen.toInt()));
        reflectControls(false);
    }
    pushEq();
    saveEq();
}

void EqualizerPage::reflectControls(bool includeCombo) {
    const dsp::EqSettings& s = current();
    {
        const QSignalBlocker block(enabledCheck_);
        enabledCheck_->setChecked(s.enabled);
    }
    if (QAbstractButton* button = bandGroup_->button(bandIndexOf(s.bandCount))) {
        button->setChecked(true);  // idClicked only fires on user clicks
    }
    if (includeCombo) {
        const QSignalBlocker block(presetCombo_);
        const int idx = presetCombo_->findData(static_cast<int>(s.preset));
        if (idx >= 0) {
            presetCombo_->setCurrentIndex(idx);
        }
    }
    curve_->setSettings(s);
}

void EqualizerPage::loadChannel() {
    reflectControls(true);
    refreshOutputs();
    reflectBass();
}

void EqualizerPage::refreshOutputs() {
    if (devices_ == nullptr || output_ == nullptr) return;
    std::vector<std::pair<QString, QString>> items;
    for (const audio::AudioDevice& dev : devices_->outputDevices()) {
        items.emplace_back(QString::fromStdString(dev.description),
                           QString::fromStdString(dev.name));
    }
    // A device that has since disappeared falls back to the first entry, which is
    // what the channel is actually doing once its target is gone.
    fillDeviceCombo(output_, QStringLiteral("Default output"), items, currentOutput_);
    devicesRevision_ = devices_->devicesRevision();
}

void EqualizerPage::showChannelOutput(const QString& deviceNodeName) {
    currentOutput_ = deviceNodeName;
    refreshOutputs();
}

ChannelId EqualizerPage::selectedChannel() const {
    return audio::kAllChannels[static_cast<std::size_t>(channel_)];
}

void EqualizerPage::showChannelGain(float gainDb, bool autoOn) {
    if (gain_ == nullptr) return;
    const QSignalBlocker g(gain_);
    const QSignalBlocker a(autoGain_);
    gain_->setValue(static_cast<int>(std::lround(gainDb)));
    gain_->setEnabled(!autoOn);
    autoGain_->setChecked(autoOn);
    gainValue_->setText(formatGainDb(gainDb, autoOn));
}

void EqualizerPage::showAutoGainValue(ChannelId id, float gainDb) {
    // The loop reports every channel; only the one on screen has anything to show.
    if (gain_ == nullptr ||
        selectedChannel() != id) {
        return;
    }
    const QSignalBlocker g(gain_);  // moving the slider must not look like a user edit
    gain_->setValue(static_cast<int>(std::lround(gainDb)));
    gainValue_->setText(formatGainDb(gainDb, true));
}

void EqualizerPage::onBandChanged(int index, float gainDb) {
    dsp::EqSettings& s = current();
    if (index >= 0 && index < static_cast<int>(s.bands.size())) {
        s.bands[index].gainDb = gainDb;
    }
    if (s.preset != dsp::EqPreset::Custom) {
        s.preset = dsp::EqPreset::Custom;
        const QSignalBlocker block(presetCombo_);
        const int idx = presetCombo_->findData(static_cast<int>(dsp::EqPreset::Custom));
        if (idx >= 0) {
            presetCombo_->setCurrentIndex(idx);
        }
    }
    eqDirty_ = true;  // flushed by eqFlush_ (throttled to avoid zipper noise)
    saveEq();         // debounced by the settings store
}

void EqualizerPage::pushEq() {
    if (controller_ != nullptr) {
        controller_->applyEqualizer(selectedChannel(),
                                    current());
    }
}

void EqualizerPage::restoreEq() {
    if (settings_ == nullptr) return;
    const QJsonObject channels =
        settings_->section(QStringLiteral("softwareEq")).value(QStringLiteral("channels")).toObject();
    if (channels.isEmpty()) return;  // first run — keep the flat defaults
    for (std::size_t i = 0; i < eq_.size(); ++i) {
        if (audio::kAllChannels[i] == ChannelId::Microphone) {
            continue;  // stored under "microphoneEq" by the microphone page
        }
        const QString key = channelKey(audio::kAllChannels[i]);
        if (channels.contains(key)) {
            eq_[i] = config::eqFromJson(channels.value(key).toObject(), eq_[i]);
        }
    }
}

void EqualizerPage::saveEq() {
    if (settings_ == nullptr) return;
    QJsonObject channels;
    for (std::size_t i = 0; i < eq_.size(); ++i) {
        if (audio::kAllChannels[i] == ChannelId::Microphone) continue;
        channels[channelKey(audio::kAllChannels[i])] =
            config::eqToJson(eq_[i]);
    }
    QJsonObject section;
    section[QStringLiteral("channels")] = channels;
    settings_->putSection(QStringLiteral("softwareEq"), section);
}

void EqualizerPage::onSavePreset() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save preset"),
                                               QStringLiteral("Preset name:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (!presets::save(name.trimmed(), current())) {
        QMessageBox::warning(this, QStringLiteral("Save preset"),
                             QStringLiteral("Could not save the preset."));
        return;
    }
    rebuildPresetCombo();
    const int idx = presetCombo_->findData(name.trimmed());
    if (idx >= 0) {
        presetCombo_->setCurrentIndex(idx);
    }
}

void EqualizerPage::onImportPreset() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import preset"),
                                                      QString(), QStringLiteral("Presets (*.json)"));
    if (path.isEmpty()) return;
    const auto name = presets::importFile(path);
    if (!name) {
        QMessageBox::warning(this, QStringLiteral("Import preset"),
                             QStringLiteral("That file is not a valid Sonero preset."));
        return;
    }
    rebuildPresetCombo();
    const int idx = presetCombo_->findData(*name);
    if (idx >= 0) {
        presetCombo_->setCurrentIndex(idx);
        onPresetSelected();
    }
}

void EqualizerPage::onExportPreset() {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export preset"),
                                                      QStringLiteral("preset.json"),
                                                      QStringLiteral("Presets (*.json)"));
    if (path.isEmpty()) return;
    if (!presets::exportFile(path, QStringLiteral("Sonero EQ"), current())) {
        QMessageBox::warning(this, QStringLiteral("Export preset"),
                             QStringLiteral("Could not write the file."));
    }
}

}  // namespace sonar::ui
