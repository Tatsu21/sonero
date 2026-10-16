#include "ui/widgets/ChannelStrip.h"

#include <QEvent>

#include "ui/Theme.h"

#include <algorithm>
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
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/widgets/AppChip.h"
#include "ui/widgets/VuMeter.h"

namespace sonar::ui {

using audio::ChannelId;
using audio::ChannelState;

namespace {

[[nodiscard]] QColor mixed(const QColor& a, const QColor& b, double t) {
    const double u = 1.0 - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t, a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t);
}

constexpr int kStripWidth = 144;
}  // namespace

ChannelStrip::ChannelStrip(ChannelId id, const QString& name, QWidget* parent)
    : QWidget(parent), id_(id) {
    setObjectName(QStringLiteral("ChannelStrip"));
    setAttribute(Qt::WA_StyledBackground, true);  // let the QSS card style paint
    setAcceptDrops(true);                         // app chips can be dropped here
    // A floor, not a fixed size. Six strips at a fixed width left a third of a
    // 1440-pixel window empty to their right, which reads as a layout that gave
    // up rather than one that was designed. The ceiling keeps a very wide window
    // from stretching six channels into six billboards.
    setMinimumWidth(kStripWidth);
    setMaximumWidth(kStripWidth * 3 / 2);

    auto* glyph = new QLabel(glyphFor(id), this);
    if (id == ChannelId::System) {
        setToolTip(QStringLiteral(
            "Master: this fader scales every other channel, and muting it "
            "silences the whole mix."));
    }
    glyph->setStyleSheet(QStringLiteral("font-size:17px;"));
    // A fixed height, because the glyphs are not all the same kind of character:
    // some channels use a colour emoji and some a plain text symbol, and the two
    // have different ascents. Left to themselves the labels size to their own
    // glyph, the header row grows for some strips and not others, and everything
    // below — fader, percentage, Mute and Solo — sits a few pixels lower on
    // Master and Aux than on the rest.
    constexpr int kHeaderHeight = 22;
    glyph->setFixedHeight(kHeaderHeight);
    glyph->setAlignment(Qt::AlignCenter);
    // System doubles as the master fader, so it is labelled for what it does
    // rather than for the streams it happens to carry.
    const bool isMaster = id == ChannelId::System;
    auto* title = new QLabel(isMaster ? QStringLiteral("MASTER") : name.toUpper(), this);
    title->setObjectName(QStringLiteral("ChannelName"));
    title_ = title;
    title->setFixedHeight(kHeaderHeight);
    title->setAlignment(Qt::AlignCenter);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(7);
    titleRow->addStretch(1);
    titleRow->addWidget(glyph);
    titleRow->addWidget(title);
    titleRow->addStretch(1);

    // A hairline in the channel's colour, as a divider under the header.
    auto* accentRule = new QFrame(this);
    accentRule->setFixedHeight(isMaster ? 3 : 2);
    accentRule_ = accentRule;

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

    // --- stream send (hidden unless stream mode is on) -----------------------
    streamRow_ = new QWidget(this);
    auto* streamCol = new QVBoxLayout(streamRow_);
    streamCol->setContentsMargins(0, 0, 0, 0);
    streamCol->setSpacing(3);
    auto* streamCaption = new QLabel(QStringLiteral("TO STREAM"), streamRow_);
    streamCaption->setObjectName(QStringLiteral("BalanceCaption"));
    streamCaption_ = streamCaption;
    stream_ = new QSlider(Qt::Horizontal, streamRow_);
    stream_->setFixedHeight(14);
    stream_->setRange(0, 100);
    stream_->setValue(100);
    stream_->setToolTip(QStringLiteral("How loud this channel is for the stream"));
    // The level reads out in the slider's tooltip; a widget with no place in the
    // layout would otherwise paint at the origin, on top of the caption.
    streamValue_ = new QLabel(streamRow_);
    streamValue_->hide();
    streamCol->addWidget(streamCaption);
    streamCol->addWidget(stream_);

    streamRow_->setStyleSheet(QStringLiteral("background:transparent;"));
    streamRow_->setVisible(false);

    connect(stream_, &QSlider::valueChanged, this, [this](int v) {
        stream_->setToolTip(QStringLiteral("To stream: %1%").arg(v));
        emit streamLevelChanged(id_, static_cast<float>(v) / 100.0f);
    });


    auto* appsHost = new QFrame(this);
    appsHost->setObjectName(QStringLiteral("AppsHost"));
    appsHost_ = appsHost;
    appsHost->setMinimumHeight(64);
    appsBody_ = new QVBoxLayout(appsHost);
    appsBody_->setContentsMargins(6, 6, 6, 6);
    appsBody_->setSpacing(5);
    appsBody_->setAlignment(Qt::AlignTop);


    appsEmpty_ = new QLabel(QStringLiteral("drop an app here"), this);
    appsEmpty_->setObjectName(QStringLiteral("Hint"));
    appsEmpty_->setAlignment(Qt::AlignHCenter);
    appsBody_->addWidget(appsEmpty_);


    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(11, 11, 11, 11);
    layout->setSpacing(6);
    layout->addLayout(titleRow);
    layout->addWidget(accentRule);
    layout->addLayout(faderRow, 2);   // meter + fader keep the bulk of the height
    layout->addWidget(volumeLabel_);
    layout->addWidget(balanceCaption);
    layout->addWidget(balance_);
    layout->addLayout(buttonRow);
    layout->addWidget(streamRow_);
    layout->addWidget(appsHost, 1);

    // Translate widget events into channel intent.
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        updateVolumeLabel(value);
        emit volumeChanged(id_, static_cast<float>(value) / 100.0f);
    });
    connect(balance_, &QSlider::valueChanged, this, [this](int value) {
        emit balanceChanged(id_, static_cast<float>(value) / 100.0f);
    });
    connect(mute_, &QPushButton::toggled, this, [this](bool checked) {
        emit muteToggled(id_, checked);
    });
    connect(solo_, &QPushButton::toggled, this, [this](bool checked) {
        emit soloToggled(id_, checked);
    });
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

    // Everything coloured by the channel's accent, in one place so a theme
    // change can re-run it.
    applyAccent();
}

void ChannelStrip::setLevel(float left, float right) {
    meter_->setLevel(left, right);
}

void ChannelStrip::applyAccent() {
    if (restyling_) {
        return;
    }
    restyling_ = true;
    const QString accent = accentFor(id_);
    if (title_ != nullptr) {
        title_->setStyleSheet(
            QStringLiteral("color:%1; font-weight:800; letter-spacing:1px;").arg(accent));
    }
    if (accentRule_ != nullptr) {
        accentRule_->setStyleSheet(
            QStringLiteral("background:%1; border-radius:1px;").arg(accent));
    }
    if (streamCaption_ != nullptr) {
        streamCaption_->setStyleSheet(QStringLiteral("color:%1;").arg(accent));
    }
    if (appsHost_ != nullptr) {
        // A well rather than a raised surface, so a chip dropped in it reads as
        // contained. Derived from the theme, which is why it is re-done here.
        appsHost_->setStyleSheet(
            QStringLiteral("#AppsHost { background:%1; border-radius:8px; }")
                .arg(mixed(currentTheme().surface, currentTheme().bg, 0.72)
                         .name(QColor::HexRgb)));
    }
    restyling_ = false;
}

void ChannelStrip::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        applyAccent();
    }
}

QString ChannelStrip::accentFor(ChannelId id) {
    // Each channel keeps its own hue, because that is identity: green has meant
    // Game since the first build, and someone glancing at the mixer finds their
    // strip by colour before they read the label.
    //
    // Saturation and lightness come from the active theme's accent instead of
    // being fixed, so the row follows the theme's mood — muted in a pastel theme,
    // vivid in a loud one — without losing which strip is which.
    int hue = 219;  // blue
    switch (id) {
        case ChannelId::System:     hue = 219; break;  // blue
        case ChannelId::Game:       hue = 142; break;  // green
        case ChannelId::Chat:       hue = 38;  break;  // amber
        case ChannelId::Media:      hue = 350; break;  // pink
        case ChannelId::Browser:    hue = 187; break;  // cyan
        case ChannelId::Microphone: hue = 48;  break;  // yellow
        case ChannelId::Aux:        hue = 256; break;  // violet
    }
    const QColor accent = currentTheme().accent;
    QColor c = QColor::fromHsl(hue, accent.hslSaturation(), accent.lightness());
    // A theme whose accent is nearly grey would turn the whole row grey, which
    // defeats the point; keep enough saturation to tell the strips apart.
    if (c.hslSaturation() < 90) {
        c = QColor::fromHsl(hue, 150, accent.lightness());
    }
    return c.name(QColor::HexRgb);
}

QString ChannelStrip::glyphFor(ChannelId id) {
    switch (id) {
        case ChannelId::System:     return QString::fromUtf8("\xF0\x9F\x8E\x9B");   // control knobs
        case ChannelId::Game:       return QString::fromUtf8("\xF0\x9F\x8E\xAE");   // gamepad
        case ChannelId::Chat:       return QString::fromUtf8("\xF0\x9F\x92\xAC");   // speech balloon
        case ChannelId::Media:      return QString::fromUtf8("\xF0\x9F\x8E\xB5");   // note
        case ChannelId::Browser:    return QString::fromUtf8("\xF0\x9F\x8C\x90");   // globe
        case ChannelId::Microphone: return QString::fromUtf8("\xF0\x9F\x8E\xA4");   // microphone
        case ChannelId::Aux:        return QString::fromUtf8("\xF0\x9F\x8E\x9A");   // fader
    }
    return QString::fromUtf8("\xF0\x9F\x8E\x9B");
}

void ChannelStrip::setStreamMode(bool on) { streamRow_->setVisible(on); }

void ChannelStrip::setStreamLevel(float level) {
    const QSignalBlocker block(stream_);
    const int v = std::clamp(static_cast<int>(std::lround(level * 100.0f)), 0, 100);
    stream_->setValue(v);
    stream_->setToolTip(QStringLiteral("To stream: %1%").arg(v));
}

void ChannelStrip::clearApps() {
    while (appsBody_->count() > 0) {
        QLayoutItem* item = appsBody_->takeAt(0);
        if (QWidget* w = item->widget()) {
            if (w == appsEmpty_) {
                w->hide();          // reused, never destroyed
            } else {
                w->deleteLater();
            }
        }
        delete item;
    }
}

void ChannelStrip::addApp(QWidget* chip) {
    appsEmpty_->hide();
    chip->setParent(nullptr);
    appsBody_->addWidget(chip);
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
