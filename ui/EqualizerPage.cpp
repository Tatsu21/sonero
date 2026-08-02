#include "ui/EqualizerPage.h"

#include <cstdlib>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/IEqualizerController.h"
#include "config/SettingsStore.h"
#include "ui/EqPresetStore.h"
#include "ui/widgets/EqCurve.h"

namespace sonar::ui {

using audio::ChannelId;

namespace {
QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

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

QLabel* caption(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("CardKey"));
    return label;
}

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
                             config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent), controller_(controller), settings_(settings) {
    for (auto& s : eq_) {
        dsp::resetBands(s, dsp::BandCount::Bands10);
        s.enabled = true;
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(18);

    auto* title = new QLabel(QStringLiteral("Equalizer"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle =
        new QLabel(QStringLiteral("Per-channel EQ — drag the curve to shape the sound"), this);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    root->addLayout(head);

    QStringList channelLabels;
    for (const ChannelId id : audio::kAllChannels) {
        channelLabels << toQString(audio::channelName(id));
    }
    root->addWidget(makeSegmented(channelLabels, channelGroup_, this));

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
    controls->addWidget(caption(QStringLiteral("Preset"), this));
    controls->addWidget(presetCombo_);
    controls->addWidget(saveBtn);
    controls->addWidget(importBtn);
    controls->addWidget(exportBtn);
    controls->addSpacing(10);
    controls->addWidget(caption(QStringLiteral("Bands"), this));
    controls->addWidget(bandSeg);
    root->addLayout(controls);

    curve_ = new EqCurve(this);
    root->addWidget(curve_, 1);

    rebuildPresetCombo();

    connect(channelGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        channel_ = id;
        loadChannel();
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
    // don't click on every mouse move.
    eqFlush_ = new QTimer(this);
    eqFlush_->setInterval(40);
    connect(eqFlush_, &QTimer::timeout, this, [this] {
        if (eqDirty_) {
            eqDirty_ = false;
            pushEq();
        }
    });
    eqFlush_->start();

    if (const char* demo = std::getenv("SONAR_EQ_DEMO")) {  // dev hook for screenshots
        const QString name = QString::fromLatin1(demo);
        for (const dsp::EqPreset preset : kBuiltinPresets) {
            if (toQString(dsp::presetName(preset)).compare(name, Qt::CaseInsensitive) == 0) {
                dsp::applyPreset(current(), preset);
                break;
            }
        }
    }

    restoreEq();  // persisted per-channel EQ overrides the defaults above

    loadChannel();
    // Apply every channel's (possibly restored) EQ to the audio path — not just
    // the visible one — so saved settings take effect immediately on launch.
    if (controller_ != nullptr) {
        for (std::size_t i = 0; i < eq_.size(); ++i) {
            controller_->applyEqualizer(audio::kAllChannels[i], eq_[i]);
        }
    }
}

dsp::EqSettings& EqualizerPage::current() {
    return eq_[static_cast<std::size_t>(channel_)];
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
    const QVariant data = presetCombo_->currentData();
    if (data.typeId() == QMetaType::QString) {
        if (const auto loaded = presets::load(data.toString())) {
            current() = *loaded;
        }
        reflectControls(false);  // keep the user preset shown in the combo
    } else {
        dsp::applyPreset(current(), static_cast<dsp::EqPreset>(data.toInt()));
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

void EqualizerPage::loadChannel() { reflectControls(true); }

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
        controller_->applyEqualizer(audio::kAllChannels[static_cast<std::size_t>(channel_)],
                                    current());
    }
}

void EqualizerPage::restoreEq() {
    if (settings_ == nullptr) {
        return;
    }
    const QJsonObject channels =
        settings_->section(QStringLiteral("softwareEq")).value(QStringLiteral("channels")).toObject();
    if (channels.isEmpty()) {
        return;  // first run — keep the flat defaults
    }
    for (std::size_t i = 0; i < eq_.size(); ++i) {
        const QString key = toQString(audio::channelName(audio::kAllChannels[i]));
        if (channels.contains(key)) {
            eq_[i] = config::eqFromJson(channels.value(key).toObject(), eq_[i]);
        }
    }
}

void EqualizerPage::saveEq() {
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject channels;
    for (std::size_t i = 0; i < eq_.size(); ++i) {
        channels[toQString(audio::channelName(audio::kAllChannels[i]))] =
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
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
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
    if (path.isEmpty()) {
        return;
    }
    const auto name = presets::importFile(path);
    if (!name) {
        QMessageBox::warning(this, QStringLiteral("Import preset"),
                             QStringLiteral("That file is not a valid LinuxSonar preset."));
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
    if (path.isEmpty()) {
        return;
    }
    if (!presets::exportFile(path, QStringLiteral("LinuxSonar EQ"), current())) {
        QMessageBox::warning(this, QStringLiteral("Export preset"),
                             QStringLiteral("Could not write the file."));
    }
}

}  // namespace sonar::ui
