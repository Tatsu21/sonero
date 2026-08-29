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
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/Channel.h"
#include "audio/IChannelController.h"
#include "config/SettingsStore.h"

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
                               config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent), controller_(controller), settings_(settings) {
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
        QStringLiteral("Your mic is a virtual source apps can select — shaping it arrives "
                       "in a later stage"), page);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    root->addLayout(head);

    // --- Input (prepared) ---
    // The wiring below is real — gain and mute reach the virtual mic and the
    // meter polls it — but nothing of it takes effect yet, so the card is inert
    // like the three below it. Re-enabling is a matter of flipping the
    // setEnabled(false) calls and starting timer_ again.
    QVBoxLayout* input = makeCard(root, QStringLiteral("Input"),
                                  QStringLiteral("Gain, mute and level of the virtual mic."),
                                  true);

    auto* deviceRow = new QHBoxLayout;
    auto* deviceKey = caption(QStringLiteral("Device"));
    deviceKey->setEnabled(false);
    deviceRow->addWidget(deviceKey);
    auto* device = new QComboBox;
    device->addItem(QStringLiteral("System default microphone"));
    // Nothing reads this yet — the mic is always the system default. A disabled
    // widget shows no tooltip, so the note that would explain it lives here.
    device->setEnabled(false);
    deviceRow->addWidget(device, 1);
    input->addLayout(deviceRow);

    gain_ = sliderRow(input, QStringLiteral("Input gain"), 0, 100, 100, QStringLiteral("%"), false);
    gainValue_ = nullptr;  // handled inside sliderRow

    auto* levelRow = new QHBoxLayout;
    auto* levelKey = caption(QStringLiteral("Input level"));
    levelKey->setEnabled(false);
    levelRow->addWidget(levelKey);
    level_ = new QProgressBar;
    level_->setEnabled(false);
    level_->setObjectName(QStringLiteral("MicLevel"));
    level_->setRange(0, 100);
    level_->setValue(0);
    level_->setTextVisible(false);
    levelRow->addWidget(level_, 1);
    input->addLayout(levelRow);

    auto* muteRow = new QHBoxLayout;
    mute_ = preparedCheck(QStringLiteral("Mute microphone"));
    muteRow->addWidget(mute_);
    muteRow->addStretch(1);
    input->addLayout(muteRow);

    // --- Noise suppression (prepared) ---
    QVBoxLayout* ns = makeCard(
        root, QStringLiteral("Noise Suppression"),
        QStringLiteral("Removes background noise (RNNoise). DSP is wired in a later stage."), true);
    ns->addWidget(preparedCheck(QStringLiteral("Enable noise suppression")));
    sliderRow(ns, QStringLiteral("Strength"), 0, 100, 70, QStringLiteral("%"), false);

    // --- Noise gate (prepared) ---
    QVBoxLayout* gate = makeCard(
        root, QStringLiteral("Noise Gate"),
        QStringLiteral("Silences the mic below a threshold — great for keyboards/fans."), true);
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

    // Restore persisted gain + mute before wiring, so setting them fires no signal.
    if (settings_ != nullptr) {
        const QJsonObject m = settings_->section(QStringLiteral("microphone"));
        if (!m.isEmpty()) {
            gain_->setValue(
                std::clamp(m.value(QStringLiteral("gain")).toInt(gain_->value()), 0, 100));
            mute_->setChecked(m.value(QStringLiteral("muted")).toBool(false));
        }
    }

    // Wire the functional controls to the real mic.
    connect(gain_, &QSlider::valueChanged, this, [this](int v) {
        if (controller_ != nullptr) {
            controller_->setChannelVolume(kMic, static_cast<float>(v) / 100.0f);
        }
        saveMic();
    });
    connect(mute_, &QCheckBox::toggled, this, [this](bool on) {
        if (controller_ != nullptr) {
            controller_->setChannelMute(kMic, on);
        }
        saveMic();
    });
    if (controller_ != nullptr) {  // apply the restored state to the real mic
        controller_->setChannelVolume(kMic, static_cast<float>(gain_->value()) / 100.0f);
        controller_->setChannelMute(kMic, mute_->isChecked());
    }

    // Left unstarted on purpose: it only drives the level meter, which is
    // disabled. Starting it would poll the controller 22 times a second to paint
    // a bar nobody can read. Start it again when the card goes live.
    timer_ = new QTimer(this);
    timer_->setInterval(45);
    connect(timer_, &QTimer::timeout, this, &MicrophonePage::refresh);
}

void MicrophonePage::refresh() {
    if (controller_ == nullptr) {
        return;
    }
    const auto lvl = controller_->channelLevel(kMic);
    const float peak = std::max(lvl.peakLeft, lvl.peakRight);
    level_->setValue(std::clamp(static_cast<int>(peak * 140.0f), 0, 100));
}

void MicrophonePage::saveMic() {
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject m;
    m[QStringLiteral("gain")] = gain_->value();
    m[QStringLiteral("muted")] = mute_->isChecked();
    settings_->putSection(QStringLiteral("microphone"), m);
}

}  // namespace sonar::ui
