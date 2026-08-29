#include "ui/DevicesPage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>

#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/IAudioDevices.h"
#include "audio/IDeviceFormats.h"
#include "config/SettingsStore.h"
#include "ui/Notifier.h"
#include "ui/widgets/EqCurve.h"

namespace sonar::ui {

namespace {

QVBoxLayout* makeCard(QVBoxLayout* root, const QString& title, const QString& subtitle) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 18, 22, 20);
    body->setSpacing(10);

    auto* t = new QLabel(title);
    t->setObjectName(QStringLiteral("SectionTitle"));
    body->addWidget(t);
    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setObjectName(QStringLiteral("Hint"));
        body->addWidget(s);
    }
    body->addSpacing(4);
    root->addWidget(frame);
    return body;
}

// A labelled battery row: caption + bar + percent. Returns the bar and percent.
void batteryRow(QVBoxLayout* body, const QString& label, QProgressBar*& bar, QLabel*& pct) {
    auto* row = new QHBoxLayout;
    auto* cap = new QLabel(label);
    cap->setObjectName(QStringLiteral("CardKey"));
    cap->setMinimumWidth(150);
    bar = new QProgressBar;
    bar->setObjectName(QStringLiteral("BatteryBar"));
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    pct = new QLabel(QStringLiteral("—"));
    pct->setObjectName(QStringLiteral("CardVal"));
    pct->setMinimumWidth(52);
    pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(cap);
    row->addWidget(bar, 1);
    row->addWidget(pct);
    body->addLayout(row);
}

// Delete every child widget / nested layout so a container can be repopulated.
void clearLayout(QLayout* layout) {
    if (layout == nullptr) {
        return;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        if (QLayout* child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

// Short badge label + accent colour for a device class.
struct TypeStyle {
    const char* label;
    const char* color;
};
TypeStyle typeStyle(audio::DeviceType t) {
    using audio::DeviceType;
    switch (t) {
        case DeviceType::Usb:       return {"USB", "#6366f1"};
        case DeviceType::Bluetooth: return {"BT", "#3b82f6"};
        case DeviceType::Hdmi:      return {"HDMI", "#a855f7"};
        case DeviceType::Spdif:     return {"S/PDIF", "#14b8a6"};
        case DeviceType::Analog:    return {"Analog", "#64748b"};
        case DeviceType::Other:     return {"Audio", "#64748b"};
    }
    return {"Audio", "#64748b"};
}

// Nominal resolution of an A2DP codec and whether it counts as hi-res. aptX HD /
// aptX Adaptive / LDAC carry 24-bit; SBC / aptX / AAC are 16-bit.
struct CodecInfo {
    const char* res;
    bool hires;
};
CodecInfo codecInfo(const std::string& codec) {
    const auto has = [&](const char* s) { return codec.find(s) != std::string::npos; };
    if (has("LDAC") || has("Adaptive")) {
        return {"up to 96kHz/24-bit", true};
    }
    if (has("HD") || has("hd")) {
        return {"48kHz/24-bit", true};  // aptX HD ceiling over classic BT
    }
    if (has("aptX") || has("aptx")) {
        return {"48kHz/16-bit", false};
    }
    return {"16-bit", false};  // SBC / SBC-XQ
}

// Quality ranking of an A2DP codec, higher = better. Used to auto-select the best
// codec a Bluetooth device offers. "HD"/"Adaptive" must be tested before plain
// "aptX", since their labels contain "aptX" too.
int codecRank(const std::string& codec) {
    const auto has = [&](const char* s) { return codec.find(s) != std::string::npos; };
    if (has("LDAC")) return 100;
    if (has("Adaptive")) return 95;              // aptX Adaptive
    if (has("HD") || has("hd")) return 90;        // aptX HD
    if (has("LC3")) return 85;                    // LE Audio
    if (has("aptX") || has("aptx")) return 70;    // plain aptX
    if (has("AAC")) return 65;
    if (has("XQ") || has("xq")) return 50;        // SBC-XQ
    if (has("SBC") || has("sbc")) return 40;
    return 10;
}

// Battery percentage of a Bluetooth output via BlueZ (org.bluez.Battery1). Needs
// BlueZ "Experimental = true"; returns nullopt when the device exposes no battery.
std::optional<int> btBatteryPercent(const std::string& nodeName) {
    const std::string prefix = "bluez_output.";
    const auto p = nodeName.find(prefix);
    if (p == std::string::npos) {
        return std::nullopt;
    }
    std::string mac = nodeName.substr(p + prefix.size());
    if (const auto dot = mac.find('.'); dot != std::string::npos) {
        mac.resize(dot);  // "80_C3_BA_94_FA_B8.1" -> "80_C3_BA_94_FA_B8"
    }
    if (mac.empty()) {
        return std::nullopt;
    }
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return std::nullopt;
    }
    for (int hci = 0; hci < 5; ++hci) {  // find the adapter the device lives under
        const QString path = QStringLiteral("/org/bluez/hci%1/dev_%2")
                                 .arg(hci)
                                 .arg(QString::fromStdString(mac));
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.bluez"), path,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        msg << QStringLiteral("org.bluez.Battery1") << QStringLiteral("Percentage");
        const QDBusMessage reply = bus.call(msg, QDBus::Block, 300);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            const int pct =
                reply.arguments().first().value<QDBusVariant>().variant().toInt();
            if (pct >= 0 && pct <= 100) {
                return pct;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

DevicesPage::DevicesPage(audio::IAudioDevices* devices, audio::IDeviceFormats* formats,
                         config::SettingsStore* settings, Notifier* notifier, QWidget* parent)
    : QWidget(parent),
      devices_(devices),
      formats_(formats),
      settings_(settings),
      notifier_(notifier) {
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
    root->setSpacing(18);

    auto* title = new QLabel(QStringLiteral("Devices"));
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("Audio outputs on this system — USB, Bluetooth, HDMI and more"));
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    root->addLayout(head);

    // Live list of every connected output, driven by the PipeWire device registry.
    QVBoxLayout* outCard = makeCard(
        root, QStringLiteral("Connected outputs"),
        QStringLiteral("Everything the system can play to. Plug in or unplug a device and it "
                       "appears or disappears here."));
    deviceListBody_ = new QVBoxLayout;
    deviceListBody_->setSpacing(8);
    outCard->addLayout(deviceListBody_);

    // Per-device transmission format (ALSA outputs only; BT uses its codec).
    buildFormatCard(root);

    // SteelSeries base-station controls, shown only while it is connected.
    buildSteelSeriesControls(root);

    root->addStretch(1);

    timer_ = new QTimer(this);  // poll the registry + refresh battery
    timer_->setInterval(1500);
    connect(timer_, &QTimer::timeout, this, &DevicesPage::syncDevices);
    timer_->start();
    syncDevices();
}

void DevicesPage::buildSteelSeriesControls(QVBoxLayout* parent) {
    steelSeriesCard_ = new QWidget;
    auto* ss = new QVBoxLayout(steelSeriesCard_);
    ss->setContentsMargins(0, 0, 0, 0);
    ss->setSpacing(18);

    const hid::ProbeResult probe = hid::SteelSeriesDevice::probe();
    if (probe.accessible) {
        device_.open();
    }

    QVBoxLayout* body = makeCard(
        ss, QStringLiteral("SteelSeries Arctis Nova Pro Wireless"),
        QStringLiteral("USB base station · onboard battery, transmission format and equalizer"));

    statusLabel_ = new QLabel(QStringLiteral("● Connected"));
    statusLabel_->setStyleSheet(QStringLiteral("color:#4ade80; font-weight:800;"));
    body->addWidget(statusLabel_);

    if (!probe.accessible) {
        auto* hint = new QLabel(QStringLiteral(
            "No permission to talk to the base station. Install the udev rule:\n"
            "  sudo cp packaging/udev/70-sonero-steelseries.rules /etc/udev/rules.d/\n"
            "  sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw"));
        hint->setObjectName(QStringLiteral("Hint"));
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(hint);
    }
    body->addSpacing(6);

    // The base station reports the headset's battery only; the spare battery in
    // the dock is not part of its status report, so it is not shown here.
    batteryRow(body, QStringLiteral("Headset battery"), headsetBar_, headsetPct_);

    if (std::getenv("SONAR_HID_DEBUG") != nullptr) {  // dev-only raw report line
        rawLabel_ = new QLabel;
        rawLabel_->setStyleSheet(
            QStringLiteral("font-family: monospace; font-size: 10px; color:#565b74;"));
        body->addSpacing(6);
        body->addWidget(rawLabel_);
    }

    // --- Headset onboard EQ (Sonar-style, written to the hardware) ---
    dsp::resetBands(headsetEq_, dsp::BandCount::Bands10);
    headsetEq_.enabled = true;
    if (settings_ != nullptr) {  // restore the persisted curve before the widgets read it
        headsetEq_ = config::eqFromJson(settings_->section(QStringLiteral("headsetEq")), headsetEq_);
    }

    // Disabled, not removed: the write path is real and the headset acknowledges
    // every report, but the curve is never applied to the audio and nobody has
    // worked out why (see hid/SKILL.md). Leaving the controls live would let the
    // user move sliders that change nothing.
    QVBoxLayout* eqBody = makeCard(
        ss, QStringLiteral("Headset EQ"),
        QStringLiteral("Unavailable — the headset accepts these writes but never applies "
                       "them. Use the equalizer on the Channels page instead."));

    struct Preset {
        const char* name;
        std::array<int, 10> gains;
    };
    static const Preset kPresets[] = {
        {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"Bass Boost", {6, 5, 3, 1, 0, 0, 0, 0, 0, 0}},
        {"Treble", {0, 0, 0, 0, 0, 1, 3, 5, 6, 6}},
        // 32 64 125 250 500 1k 2k 4k 8k 16k
        {"Sonar", {0, 0, 1, 1, 0, 2, 0, 4, 3, 3}},
    };
    auto* presetRow = new QHBoxLayout;
    presetRow->setSpacing(8);
    for (const Preset& p : kPresets) {
        auto* button = new QPushButton(QString::fromUtf8(p.name));
        button->setEnabled(false);
        const std::array<int, 10> gains = p.gains;
        connect(button, &QPushButton::clicked, this, [this, gains] { applyHeadsetPreset(gains); });
        presetRow->addWidget(button);
    }
    presetRow->addStretch(1);
    eqBody->addLayout(presetRow);

    eqCurve_ = new EqCurve;
    eqCurve_->setEnabled(false);  // stops the drag and greys the curve
    eqCurve_->setMinimumHeight(150);
    eqCurve_->setSettings(headsetEq_);
    eqBody->addWidget(eqCurve_);
    connect(eqCurve_, &EqCurve::bandChanged, this, &DevicesPage::onEqBandChanged);

    // Precise per-band sliders (32 Hz .. 16 kHz), synced with the curve.
    static const char* const kFreqLabels[10] = {"32", "64",  "125", "250", "500",
                                                "1k", "2k", "4k",  "8k",  "16k"};
    auto* sliderRow = new QHBoxLayout;
    sliderRow->setSpacing(4);
    for (int i = 0; i < 10; ++i) {
        auto* col = new QVBoxLayout;
        col->setSpacing(4);

        // Initial value comes from the (possibly restored) model. Set before the
        // valueChanged connect below so it doesn't trigger a spurious HID write.
        const int g = i < static_cast<int>(headsetEq_.bands.size())
                          ? static_cast<int>(std::lround(headsetEq_.bands[static_cast<std::size_t>(i)].gainDb))
                          : 0;

        auto* value = new QLabel(QString::number(g));
        value->setObjectName(QStringLiteral("VolumeValue"));
        value->setAlignment(Qt::AlignHCenter);
        value->setEnabled(false);  // the whole band column reads as inert

        auto* slider = new QSlider(Qt::Vertical);
        slider->setEnabled(false);
        slider->setRange(-10, 10);
        slider->setValue(g);
        slider->setMinimumHeight(110);

        auto* freq = new QLabel(QString::fromUtf8(kFreqLabels[i]));
        freq->setObjectName(QStringLiteral("BalanceCaption"));
        freq->setAlignment(Qt::AlignHCenter);
        freq->setEnabled(false);

        col->addWidget(value);
        col->addWidget(slider, 1, Qt::AlignHCenter);
        col->addWidget(freq);
        sliderRow->addLayout(col);

        eqSliders_[static_cast<std::size_t>(i)] = slider;
        eqValues_[static_cast<std::size_t>(i)] = value;
        connect(slider, &QSlider::valueChanged, this,
                [this, i](int v) { setBand(i, static_cast<float>(v)); });
    }
    eqBody->addSpacing(16);
    eqBody->addLayout(sliderRow);

    eqTimer_ = new QTimer(this);  // debounce: write once after the user settles
    eqTimer_->setSingleShot(true);
    connect(eqTimer_, &QTimer::timeout, this, [this] { sendHeadsetEq(); });

    parent->addWidget(steelSeriesCard_);
}

void DevicesPage::syncDevices() {
    if (devices_ != nullptr) {
        const std::uint64_t rev = devices_->devicesRevision();
        if (rev != lastDevicesRev_) {
            lastDevicesRev_ = rev;
            clearLayout(deviceListBody_);
            btBatteryLabels_.clear();  // labels were just deleted with the rows
            const std::vector<audio::AudioDevice> list = devices_->outputDevices();
            // Forget auto-prefer state for devices that went away, so a reconnect
            // re-applies the best codec.
            {
                std::unordered_set<std::string> present;
                for (const audio::AudioDevice& d : list) {
                    present.insert(d.name);
                }
                for (auto it = autoPreferredDevices_.begin(); it != autoPreferredDevices_.end();) {
                    it = present.count(*it) != 0 ? std::next(it)
                                                 : autoPreferredDevices_.erase(it);
                }
            }
            // Notify on devices (dis)connecting — edge-triggered, silent on first fill.
            {
                std::unordered_map<std::string, std::string> current;
                for (const audio::AudioDevice& d : list) {
                    current[d.name] = d.description;
                }
                if (devicesInitialized_ && notifier_ != nullptr) {
                    for (const auto& [name, desc] : current) {
                        if (knownDevices_.find(name) == knownDevices_.end()) {
                            notifier_->notify(QStringLiteral("Device connected"),
                                              QString::fromStdString(desc), Notifier::Low);
                        }
                    }
                    for (const auto& [name, desc] : knownDevices_) {
                        if (current.find(name) == current.end()) {
                            notifier_->notify(QStringLiteral("Device disconnected"),
                                              QString::fromStdString(desc), Notifier::Low);
                        }
                    }
                }
                knownDevices_ = std::move(current);
                devicesInitialized_ = true;
            }
            if (list.empty()) {
                auto* none = new QLabel(QStringLiteral("No audio outputs detected."));
                none->setObjectName(QStringLiteral("Hint"));
                deviceListBody_->addWidget(none);
            } else {
                for (const audio::AudioDevice& dev : list) {
                    buildDeviceRow(deviceListBody_, dev);
                }
            }
            syncFormatDevices();  // keep the format selector in step with the list
        }
    }

    // Refresh Bluetooth battery levels in place (they drift over time).
    for (auto& [label, nodeName] : btBatteryLabels_) {
        updateBtBattery(label, nodeName);
    }

    // The SteelSeries HID controls follow the base station's USB presence.
    const bool present = hid::SteelSeriesDevice::probe().present;
    if (steelSeriesCard_ != nullptr) {
        steelSeriesCard_->setVisible(present);
    }
    if (present) {
        if (!device_.isOpen()) {
            device_.open();
        }
        refresh();
    } else {
        device_.close();
    }
}

void DevicesPage::buildDeviceRow(QVBoxLayout* body, const audio::AudioDevice& dev) {
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("DeviceRow"));
    row->setStyleSheet(QStringLiteral("#DeviceRow { background:#12131b; border-radius:10px; }"));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(12);

    const TypeStyle st = typeStyle(dev.type);
    auto* badge = new QLabel(QString::fromUtf8(st.label));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedWidth(64);
    badge->setStyleSheet(QStringLiteral("background:%1; color:white; font-weight:800; "
                                        "font-size:11px; border-radius:7px; padding:5px 0;")
                             .arg(QString::fromUtf8(st.color)));

    auto* name = new QLabel(QString::fromStdString(dev.description));
    name->setStyleSheet(QStringLiteral("font-weight:700; font-size:13px;"));
    auto* sub = new QLabel(QString::fromStdString(dev.name));
    sub->setObjectName(QStringLiteral("Hint"));
    sub->setStyleSheet(QStringLiteral("font-size:11px;"));
    auto* texts = new QVBoxLayout;
    texts->setSpacing(1);
    texts->addWidget(name);
    texts->addWidget(sub);

    h->addWidget(badge);
    h->addLayout(texts, 1);

    // Output volume for this device (fixes "too quiet" without leaving the app).
    if (devices_ != nullptr) {
        auto* vol = new QSlider(Qt::Horizontal);
        vol->setRange(0, 100);
        vol->setFixedWidth(96);
        const int cur =
            static_cast<int>(std::lround(devices_->deviceVolume(dev.nodeId) * 100.0f));
        vol->setValue(std::clamp(cur, 0, 100));
        auto* pct = new QLabel(QStringLiteral("%1%").arg(vol->value()));
        pct->setStyleSheet(QStringLiteral("color:#9aa0b5; font-size:11px;"));
        pct->setFixedWidth(34);
        const std::uint32_t nid = dev.nodeId;
        connect(vol, &QSlider::valueChanged, this, [this, nid, pct](int v) {
            pct->setText(QStringLiteral("%1%").arg(v));
            if (devices_ != nullptr) {
                devices_->setDeviceVolume(nid, static_cast<float>(v) / 100.0f);
            }
        });
        h->addWidget(vol);
        h->addWidget(pct);
    }

    // Bluetooth: let the user pick the A2DP codec live (aptX / aptX HD / SBC …).
    if (dev.type == audio::DeviceType::Bluetooth && dev.deviceId != 0 && devices_ != nullptr) {
        audio::DeviceCodecs cc = devices_->deviceCodecs(dev.deviceId);
        // Auto-prefer the highest-quality codec once, the first time we see this
        // device connected. Manual changes afterwards are respected (the name stays
        // marked until the device disconnects).
        if (cc.valid && autoPreferredDevices_.insert(dev.name).second) {
            int bestRank = -1;
            std::uint32_t bestIdx = cc.currentIndex;
            for (const audio::DeviceCodec& c : cc.codecs) {
                if (!c.available) {
                    continue;
                }
                if (const int r = codecRank(c.codec); r > bestRank) {
                    bestRank = r;
                    bestIdx = c.profileIndex;
                }
            }
            if (bestIdx != cc.currentIndex) {
                devices_->setDeviceProfile(dev.deviceId, bestIdx);
                cc.currentIndex = bestIdx;  // reflect it in the combo below
            }
        }
        if (cc.valid) {
            auto* cap = new QLabel(QStringLiteral("Codec"));
            cap->setObjectName(QStringLiteral("Hint"));
            auto* combo = new QComboBox;
            combo->setMinimumWidth(150);
            for (const audio::DeviceCodec& c : cc.codecs) {
                const CodecInfo info = codecInfo(c.codec);
                QString label =
                    QStringLiteral("%1 · %2").arg(QString::fromStdString(c.codec),
                                                  QString::fromUtf8(info.res));
                if (!c.available) {
                    label += QStringLiteral(" (n/a)");
                }
                combo->addItem(label, static_cast<uint>(c.profileIndex));
            }
            if (const int i = combo->findData(static_cast<uint>(cc.currentIndex)); i >= 0) {
                combo->setCurrentIndex(i);
            }
            // "HI-RES" badge lit whenever the active codec carries 24-bit audio.
            auto* hires = new QLabel(QStringLiteral("HI-RES"));
            hires->setStyleSheet(
                QStringLiteral("background:#f59e0b; color:#12131b; font-weight:800; "
                               "font-size:9px; border-radius:5px; padding:3px 6px;"));
            hires->setVisible(combo->currentText().contains(QStringLiteral("24-bit")));
            const std::uint32_t did = dev.deviceId;
            connect(combo, &QComboBox::activated, this, [this, did, combo, hires](int) {
                if (devices_ != nullptr) {
                    devices_->setDeviceProfile(did, combo->currentData().toUInt());
                }
                hires->setVisible(combo->currentText().contains(QStringLiteral("24-bit")));
            });
            h->addWidget(cap);
            h->addWidget(combo);
            h->addWidget(hires);
        }
    }

    // Bluetooth battery (via BlueZ; hidden when the device exposes none).
    if (dev.type == audio::DeviceType::Bluetooth) {
        auto* bat = new QLabel;
        updateBtBattery(bat, dev.name);
        btBatteryLabels_.push_back({bat, dev.name});
        h->addWidget(bat);
    }

    auto* dot = new QLabel(QStringLiteral("● live"));
    dot->setStyleSheet(QStringLiteral("color:#4ade80; font-weight:700; font-size:11px;"));
    h->addWidget(dot);
    body->addWidget(row);
}

void DevicesPage::updateBtBattery(QLabel* label, const std::string& nodeName) {
    const std::optional<int> pct = btBatteryPercent(nodeName);
    if (!pct.has_value()) {
        label->setVisible(false);
        return;
    }
    const int p = *pct;
    const char* color = p <= 20 ? "#ff5c7a" : (p <= 40 ? "#facc15" : "#4ade80");
    label->setText(QStringLiteral("🔋 %1%").arg(p));
    label->setStyleSheet(
        QStringLiteral("color:%1; font-weight:700; font-size:12px;").arg(QString::fromUtf8(color)));
    label->setVisible(true);

    // Low-battery notification, once per drain (rearmed above 25%).
    if (p <= 20) {
        if (btLowWarned_.insert(nodeName).second && notifier_ != nullptr) {
            const auto it = knownDevices_.find(nodeName);
            const QString what = it != knownDevices_.end() ? QString::fromStdString(it->second)
                                                           : QStringLiteral("Bluetooth device");
            notifier_->notify(QStringLiteral("Battery low"),
                              QStringLiteral("%1 at %2%").arg(what).arg(p), Notifier::Critical,
                              QStringLiteral("battery-caution"));
        }
    } else if (p > 25) {
        btLowWarned_.erase(nodeName);
    }
}

void DevicesPage::buildFormatCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Transmission format"),
        QStringLiteral("Sample rate and bit depth an ALSA output's DAC is fed at. "
                       "Bluetooth outputs use their codec instead (set it on the device "
                       "above)."));

    formatDeviceCombo_ = new QComboBox;
    connect(formatDeviceCombo_, &QComboBox::activated, this,
            [this](int) { loadFormatForSelectedDevice(); });
    auto* devCap = new QLabel(QStringLiteral("Device"));
    devCap->setObjectName(QStringLiteral("CardKey"));
    auto* devRow = new QHBoxLayout;
    devRow->setSpacing(8);
    devRow->addWidget(devCap);
    devRow->addWidget(formatDeviceCombo_, 1);
    body->addLayout(devRow);

    rateCombo_ = new QComboBox;
    depthCombo_ = new QComboBox;

    auto* apply = new QPushButton(QStringLiteral("Save"));
    apply->setCursor(Qt::PointingHandCursor);
    connect(apply, &QPushButton::clicked, this, &DevicesPage::applyFormat);

    auto* rateCap = new QLabel(QStringLiteral("Sample rate"));
    rateCap->setObjectName(QStringLiteral("CardKey"));
    auto* depthCap = new QLabel(QStringLiteral("Bit depth"));
    depthCap->setObjectName(QStringLiteral("CardKey"));

    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(rateCap);
    row->addWidget(rateCombo_);
    row->addSpacing(16);
    row->addWidget(depthCap);
    row->addWidget(depthCombo_);
    row->addStretch(1);
    row->addWidget(apply);
    body->addLayout(row);

    formatStatus_ = new QLabel;
    formatStatus_->setObjectName(QStringLiteral("Hint"));
    formatStatus_->setWordWrap(true);
    body->addWidget(formatStatus_);

    syncFormatDevices();  // fill from whatever is already connected
}

void DevicesPage::syncFormatDevices() {
    if (formatDeviceCombo_ == nullptr || devices_ == nullptr) {
        return;
    }
    const QString prev = formatDeviceCombo_->currentData().toString();
    {
        const QSignalBlocker block(formatDeviceCombo_);
        formatDeviceCombo_->clear();
        for (const audio::AudioDevice& d : devices_->outputDevices()) {
            if (d.type == audio::DeviceType::Bluetooth) {
                continue;  // Bluetooth format == codec, handled per device row
            }
            formatDeviceCombo_->addItem(QString::fromStdString(d.description),
                                        QString::fromStdString(d.name));
        }
        const int idx = formatDeviceCombo_->findData(prev);
        formatDeviceCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    if (formatDeviceCombo_->count() == 0) {
        rateCombo_->clear();
        depthCombo_->clear();
        formatStatus_->setText(QStringLiteral("No ALSA output devices connected."));
        formatStatus_->setStyleSheet(QString());
        return;
    }
    loadFormatForSelectedDevice();
}

void DevicesPage::loadFormatForSelectedDevice() {
    if (formatDeviceCombo_ == nullptr) {
        return;
    }
    const QString nodeName = formatDeviceCombo_->currentData().toString();
    if (nodeName.isEmpty()) {
        return;
    }
    const std::string name = nodeName.toStdString();

    // Offer ONLY what the device actually supports. Forcing an unsupported rate
    // (e.g. 96 kHz on a 48 kHz-only DAC) makes the ALSA node fail to open and the
    // device vanishes — ALSA does not silently clamp it. Fall back to the standard
    // ladder only when the device could not be queried.
    audio::DeviceFormats caps;
    if (formats_ != nullptr) {
        caps = formats_->supportedFormats(name);
    }
    const std::vector<int> rates = (caps.valid && !caps.rates.empty())
                                       ? caps.rates
                                       : audio::OutputFormatManager::supportedRates();
    const std::vector<int> depths = (caps.valid && !caps.depths.empty())
                                        ? caps.depths
                                        : audio::OutputFormatManager::supportedDepths();

    {
        const QSignalBlocker rb(rateCombo_);
        rateCombo_->clear();
        for (const int r : rates) {
            rateCombo_->addItem(QStringLiteral("%1 kHz").arg(r / 1000.0, 0, 'g', 5), r);
        }
        const QSignalBlocker db(depthCombo_);
        depthCombo_->clear();
        for (const int d : depths) {
            depthCombo_->addItem(QStringLiteral("%1-bit").arg(d), d);
        }
    }

    const audio::OutputFormatManager mgr(name);
    const audio::OutputFormat cur = mgr.current().value_or(audio::OutputFormat{});
    if (const int i = rateCombo_->findData(cur.rateHz); i >= 0) rateCombo_->setCurrentIndex(i);
    if (const int i = depthCombo_->findData(cur.bitDepth); i >= 0) depthCombo_->setCurrentIndex(i);

    formatStatus_->setText(
        caps.valid
            ? QStringLiteral("Only formats %1 actually supports are shown, so it can't be "
                             "pushed into an unopenable mode.")
                  .arg(formatDeviceCombo_->currentText())
            : QStringLiteral("Standard rate / depth ladder (device not queried live)."));
    formatStatus_->setStyleSheet(QString());
}

void DevicesPage::applyFormat() {
    if (formatDeviceCombo_ == nullptr) {
        return;
    }
    const QString nodeName = formatDeviceCombo_->currentData().toString();
    if (nodeName.isEmpty()) {
        return;
    }
    audio::OutputFormat fmt;
    fmt.rateHz = rateCombo_->currentData().toInt();
    fmt.bitDepth = depthCombo_->currentData().toInt();

    audio::OutputFormatManager mgr(nodeName.toStdString());
    const bool wrote = mgr.apply(fmt);
    const QString what =
        QStringLiteral("%1-bit / %2 kHz").arg(fmt.bitDepth).arg(fmt.rateHz / 1000.0, 0, 'g', 5);

    if (!wrote) {
        formatStatus_->setText(QStringLiteral("Could not write %1.")
                                   .arg(QString::fromStdString(mgr.configPath())));
        formatStatus_->setStyleSheet(QStringLiteral("color:#ff5c7a;"));
    } else {
        formatStatus_->setText(
            QStringLiteral("Saved %1 for %2 — applies at next login (or `systemctl --user "
                           "restart wireplumber`). Bluetooth is not disturbed.")
                .arg(what, formatDeviceCombo_->currentText()));
        formatStatus_->setStyleSheet(QStringLiteral("color:#4ade80;"));
    }
}

void DevicesPage::onEqBandChanged(int index, float gainDb) { setBand(index, gainDb); }

void DevicesPage::setBand(int index, float gainDb) {
    if (index < 0 || index >= 10) {
        return;
    }
    const auto i = static_cast<std::size_t>(index);
    const int g = static_cast<int>(std::lround(gainDb));  // hardware is ~1 dB steps
    headsetEq_.bands[i].gainDb = static_cast<float>(g);
    {
        const QSignalBlocker block(eqSliders_[i]);
        eqSliders_[i]->setValue(g);
    }
    eqValues_[i]->setText(QString::number(g));
    eqCurve_->setSettings(headsetEq_);
    eqTimer_->start(250);  // debounced write to the headset
    saveHeadsetEq();       // debounced persistence to disk
}

void DevicesPage::saveHeadsetEq() {
    if (settings_ != nullptr) {
        settings_->putSection(QStringLiteral("headsetEq"), config::eqToJson(headsetEq_));
    }
}

void DevicesPage::applyHeadsetPreset(const std::array<int, 10>& gainsDb) {
    for (int i = 0; i < 10 && i < static_cast<int>(gainsDb.size()); ++i) {
        setBand(i, static_cast<float>(gainsDb[i]));  // (re)starts the debounce
    }
}

void DevicesPage::sendHeadsetEq() {
    std::array<int, 10> gains{};
    for (std::size_t i = 0; i < gains.size() && i < headsetEq_.bands.size(); ++i) {
        gains[i] = static_cast<int>(std::lround(headsetEq_.bands[i].gainDb));
    }
    device_.setEqualizer(gains);
}

void DevicesPage::refresh() {
    const hid::BatteryStatus s = device_.readBattery();
    if (!s.valid) {
        statusLabel_->setText(QStringLiteral("● No response"));
        statusLabel_->setStyleSheet(QStringLiteral("color:#ff5c7a; font-weight:800;"));
        return;
    }
    // The dock stays reachable even when the headset itself is powered off, so
    // report the headset's link state rather than just "connected".
    switch (s.state) {
        case hid::HeadsetState::Offline:
            statusLabel_->setText(QStringLiteral("● Headset off"));
            statusLabel_->setStyleSheet(QStringLiteral("color:#facc15; font-weight:800;"));
            break;
        case hid::HeadsetState::Charging:
            statusLabel_->setText(QStringLiteral("● Charging"));
            statusLabel_->setStyleSheet(QStringLiteral("color:#4ade80; font-weight:800;"));
            break;
        default:
            statusLabel_->setText(QStringLiteral("● Connected"));
            statusLabel_->setStyleSheet(QStringLiteral("color:#4ade80; font-weight:800;"));
            break;
    }

    const auto apply = [](QProgressBar* bar, QLabel* pct, int value) {
        bar->setValue(value < 0 ? 0 : value);
        pct->setText(value < 0 ? QStringLiteral("—") : QStringLiteral("%1%").arg(value));
    };
    apply(headsetBar_, headsetPct_, s.headsetPercent);

    // Low-battery notifications, edge-triggered with hysteresis (warn ≤20%, rearm >25%).
    const auto lowBattery = [this](int pct, bool& warned, const QString& what) {
        if (pct >= 0 && pct <= 20 && !warned) {
            warned = true;
            if (notifier_ != nullptr) {
                notifier_->notify(QStringLiteral("Battery low"),
                                  QStringLiteral("%1 at %2%").arg(what).arg(pct),
                                  Notifier::Critical, QStringLiteral("battery-caution"));
            }
        } else if (pct > 25) {
            warned = false;
        }
    };
    lowBattery(s.headsetPercent, headsetLowWarned_, QStringLiteral("Headset"));

    if (rawLabel_ != nullptr) {
        QString raw = QStringLiteral("raw: ");
        for (int i = 0; i < 16; ++i) {
            raw += QStringLiteral("%1 ").arg(s.raw[static_cast<std::size_t>(i)], 2, 16,
                                             QLatin1Char('0'));
        }
        rawLabel_->setText(raw.trimmed());
    }
}

}  // namespace sonar::ui
