#include "ui/widgets/ChannelStrip.h"

#include <cmath>

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/widgets/AppChip.h"
#include "ui/widgets/VuMeter.h"

namespace sonar::ui {

using audio::ChannelId;
using audio::ChannelState;

namespace {
constexpr int kStripWidth = 144;
}  // namespace

ChannelStrip::ChannelStrip(ChannelId id, const QString& name, QWidget* parent)
    : QWidget(parent), id_(id) {
    setObjectName(QStringLiteral("ChannelStrip"));
    setAttribute(Qt::WA_StyledBackground, true);  // let the QSS card style paint
    setAcceptDrops(true);                         // app chips can be dropped here
    setFixedWidth(kStripWidth);

    auto* title = new QLabel(name, this);
    title->setObjectName(QStringLiteral("ChannelName"));
    title->setAlignment(Qt::AlignHCenter);

    meter_ = new VuMeter(this);

    volume_ = new QSlider(Qt::Vertical, this);
    volume_->setRange(0, 100);
    volume_->setValue(75);

    auto* faderRow = new QHBoxLayout;
    faderRow->setSpacing(10);
    faderRow->addStretch(1);
    faderRow->addWidget(meter_);
    faderRow->addWidget(volume_);
    faderRow->addStretch(1);

    volumeLabel_ = new QLabel(this);
    volumeLabel_->setObjectName(QStringLiteral("VolumeValue"));
    volumeLabel_->setAlignment(Qt::AlignHCenter);
    updateVolumeLabel(volume_->value());

    auto* balanceCaption = new QLabel(QStringLiteral("BALANCE"), this);
    balanceCaption->setObjectName(QStringLiteral("BalanceCaption"));
    balanceCaption->setAlignment(Qt::AlignHCenter);

    balance_ = new QSlider(Qt::Horizontal, this);
    balance_->setRange(-100, 100);
    balance_->setValue(0);
    balance_->setToolTip(QStringLiteral("Balance (L / R)"));

    auto* gainCaption = new QLabel(QStringLiteral("GAIN"), this);
    gainCaption->setObjectName(QStringLiteral("BalanceCaption"));
    gainCaption->setAlignment(Qt::AlignHCenter);

    // Trim in dB, separate from the fader: it tames a source that is far louder
    // than the others without giving up fader travel. Cuts go deeper than boosts
    // because boosting a full-scale source is what clips in the first place.
    gain_ = new QSlider(Qt::Horizontal, this);
    gain_->setRange(-20, 6);
    gain_->setValue(0);
    gain_->setToolTip(QStringLiteral("Input trim in dB (0 dB = unchanged)"));

    autoGain_ = new QCheckBox(QStringLiteral("Auto"), this);
    autoGain_->setToolTip(QStringLiteral(
        "Track the level of what is playing and trim it automatically.\n"
        "Pulls loud passages down quickly, lifts quiet ones slowly, and holds\n"
        "still during silence."));

    gainValue_ = new QLabel(QStringLiteral("0 dB"), this);
    gainValue_->setObjectName(QStringLiteral("BalanceCaption"));
    gainValue_->setAlignment(Qt::AlignHCenter);

    mute_ = new QPushButton(QStringLiteral("Mute"), this);
    mute_->setCheckable(true);
    mute_->setProperty("toggleRole", QStringLiteral("mute"));

    solo_ = new QPushButton(QStringLiteral("Solo"), this);
    solo_->setCheckable(true);
    solo_->setProperty("toggleRole", QStringLiteral("solo"));

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(6);
    buttonRow->addWidget(mute_);
    buttonRow->addWidget(solo_);

    auto* outputCaption = new QLabel(QStringLiteral("OUTPUT"), this);
    outputCaption->setObjectName(QStringLiteral("BalanceCaption"));
    outputCaption->setAlignment(Qt::AlignHCenter);

    output_ = new QComboBox(this);
    output_->setToolTip(QStringLiteral("Output device for this channel"));
    output_->addItem(QStringLiteral("Default"), QString());
    output_->setMaximumWidth(kStripWidth - 24);

    appsLabel_ = new QLabel(this);
    appsLabel_->setObjectName(QStringLiteral("AppsLabel"));
    appsLabel_->setWordWrap(true);
    appsLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    appsLabel_->setMinimumHeight(30);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 14, 12, 14);
    layout->setSpacing(9);
    layout->addWidget(title);
    layout->addLayout(faderRow, 1);
    layout->addWidget(volumeLabel_);
    layout->addWidget(balanceCaption);
    layout->addWidget(balance_);
    auto* gainHead = new QHBoxLayout;
    gainHead->setContentsMargins(0, 0, 0, 0);
    gainHead->addWidget(gainCaption, 1);
    gainHead->addWidget(autoGain_);
    layout->addLayout(gainHead);
    layout->addWidget(gain_);
    layout->addWidget(gainValue_);
    layout->addLayout(buttonRow);
    layout->addWidget(outputCaption);
    layout->addWidget(output_);
    layout->addWidget(appsLabel_);

    // Translate widget events into channel intent.
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        updateVolumeLabel(value);
        emit volumeChanged(id_, static_cast<float>(value) / 100.0f);
    });
    connect(balance_, &QSlider::valueChanged, this, [this](int value) {
        emit balanceChanged(id_, static_cast<float>(value) / 100.0f);
    });
    connect(gain_, &QSlider::valueChanged, this, [this](int db) {
        gainValue_->setText(db > 0 ? QStringLiteral("+%1 dB").arg(db)
                                   : QStringLiteral("%1 dB").arg(db));
        emit gainChanged(id_, static_cast<float>(db));
    });
    connect(autoGain_, &QCheckBox::toggled, this, [this](bool on) {
        gain_->setEnabled(!on);  // the loop owns the value while it is running
        emit autoGainToggled(id_, on);
    });
    connect(mute_, &QPushButton::toggled, this, [this](bool checked) {
        emit muteToggled(id_, checked);
    });
    connect(solo_, &QPushButton::toggled, this, [this](bool checked) {
        emit soloToggled(id_, checked);
    });
    connect(output_, &QComboBox::activated, this, [this](int) {
        emit outputChanged(id_, output_->currentData().toString());
    });
}

void ChannelStrip::setOutputDevices(const QList<QPair<QString, QString>>& devices,
                                    const QString& currentNodeName) {
    const QSignalBlocker block(output_);
    output_->clear();
    for (const auto& [label, nodeName] : devices) {
        output_->addItem(label, nodeName);
    }
    int idx = output_->findData(currentNodeName);
    if (idx < 0 && !currentNodeName.isEmpty()) {
        // Pinned to a device that is not connected right now — keep the choice
        // visible instead of silently snapping back to Default.
        output_->addItem(currentNodeName + QStringLiteral(" (offline)"), currentNodeName);
        idx = output_->count() - 1;
    }
    output_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void ChannelStrip::showGainDb(float gainDb) {
    const int db = static_cast<int>(std::lround(gainDb));
    const QSignalBlocker block(gain_);
    gain_->setValue(db);
    gainValue_->setText(db > 0 ? QStringLiteral("+%1 dB").arg(db)
                               : QStringLiteral("%1 dB").arg(db));
}

void ChannelStrip::setAutoGain(bool on) {
    const QSignalBlocker block(autoGain_);
    autoGain_->setChecked(on);
    gain_->setEnabled(!on);
}

void ChannelStrip::setState(const ChannelState& state) {
    // Block signals so reflecting model state does not echo back as user intent.
    const QSignalBlocker vb(volume_);
    const QSignalBlocker bb(balance_);
    const QSignalBlocker mb(mute_);
    const QSignalBlocker sb(solo_);

    volume_->setValue(static_cast<int>(state.volume * 100.0f));
    balance_->setValue(static_cast<int>(state.balance * 100.0f));
    mute_->setChecked(state.muted);
    solo_->setChecked(state.solo);
    updateVolumeLabel(volume_->value());
}

void ChannelStrip::setLevel(float left, float right) {
    meter_->setLevel(left, right);
}

void ChannelStrip::setAssignedApps(const QStringList& appNames) {
    appsLabel_->setText(appNames.isEmpty() ? QStringLiteral("—")
                                           : appNames.join(QStringLiteral("\n")));
}

void ChannelStrip::setDropActive(bool active) {
    setProperty("dropActive", active);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ChannelStrip::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(QString::fromLatin1(kAppMimeType))) {
        event->acceptProposedAction();
        setDropActive(true);
    }
}

void ChannelStrip::dragLeaveEvent(QDragLeaveEvent* /*event*/) {
    setDropActive(false);
}

void ChannelStrip::dropEvent(QDropEvent* event) {
    setDropActive(false);
    if (!event->mimeData()->hasFormat(QString::fromLatin1(kAppMimeType))) {
        return;
    }
    event->acceptProposedAction();
    // Read the id from AppChip (not the MIME pipe) to avoid Wayland self-drop
    // read timeouts.
    if (const std::uint32_t appId = AppChip::draggedAppId(); appId != 0) {
        emit appDropped(id_, appId);
    }
}

void ChannelStrip::updateVolumeLabel(int percent) {
    volumeLabel_->setText(QStringLiteral("%1%").arg(percent));
}

}  // namespace sonar::ui
