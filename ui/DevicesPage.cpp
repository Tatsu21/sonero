#include "ui/DevicesPage.h"

#include "hid/BatterySource.h"
#include "ui/UiKit.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "app/SystemSetup.h"
#include "audio/IAudioDevices.h"
#include "audio/IDeviceFormats.h"
#include "config/SettingsStore.h"
#include "ui/Notifier.h"

namespace sonar::ui {

namespace {
// A device-type badge: the hue is the type's identity, saturation and lightness
// follow the theme's accent. `muted` is for the unremarkable types.
[[nodiscard]] QString badgeColor(int hue, bool muted = false) {
    const QColor accent = currentTheme().accent;
    const int sat = muted ? accent.hslSaturation() / 4 : accent.hslSaturation();
    return QColor::fromHsl(hue, std::clamp(sat, 24, 255), accent.lightness())
        .name(QColor::HexRgb);
}

// A labelled battery row: caption + bar + percent. Returns the bar and percent.
void batteryRow(QVBoxLayout* body, const QString& label, QProgressBar*& bar, QLabel*& pct) {
    auto* row = new QHBoxLayout;
    QLabel* cap = makeLabel(label, "CardKey");
    cap->setMinimumWidth(150);
    bar = makeLevelBar("BatteryBar");
    pct = makeLabel(QStringLiteral("—"), "CardVal");
    pct->setMinimumWidth(52);
    pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(cap);
    row->addWidget(bar, 1);
    row->addWidget(pct);
    body->addLayout(row);
}

// Short badge label + accent colour for a device class.
struct TypeStyle {
    const char* label;
    // A QString rather than a literal: the colour is derived from the active
    // theme now, so it is computed per call rather than baked in.
    QString color;
};
TypeStyle typeStyle(audio::DeviceType t) {
    using audio::DeviceType;
    switch (t) {
        case DeviceType::Usb:       return {"USB", badgeColor(239)};
        case DeviceType::Bluetooth: return {"BT", badgeColor(217)};
        case DeviceType::Hdmi:      return {"HDMI", badgeColor(280)};
        case DeviceType::Spdif:     return {"S/PDIF", badgeColor(174)};
        case DeviceType::Analog:    return {"Analog", badgeColor(215, true)};
        // Nothing is classified System today; listed so -Wswitch catches a new class.
        case DeviceType::System:
        case DeviceType::Other:     return {"Audio", badgeColor(215, true)};
    }
    return {"Audio", badgeColor(215, true)};
}

// Nominal resolution of an A2DP codec and whether it counts as hi-res. aptX HD /
// aptX Adaptive / LDAC carry 24-bit; SBC / aptX / AAC are 16-bit.
struct CodecInfo {
    const char* res;
    bool hires;
};
CodecInfo codecInfo(const std::string& codec) {
    const auto has = [&](const char* s) { return codec.find(s) != std::string::npos; };
    if (has("LDAC") || has("Adaptive")) return {"up to 96kHz/24-bit", true};
    if (has("HD") || has("hd")) return {"48kHz/24-bit", true};  // aptX HD ceiling over classic BT
    if (has("aptX") || has("aptx")) return {"48kHz/16-bit", false};
    return {"16-bit", false};  // SBC / SBC-XQ
}

// A2DP codec quality, higher = better, for auto-selecting the best. "HD" and
// "Adaptive" before plain "aptX", whose name they contain.
int codecRank(const std::string& codec) {
    const auto has = [&](const char* s) { return codec.find(s) != std::string::npos; };
    if (has("LDAC")) return 100;
    if (has("Adaptive")) return 95;              // aptX Adaptive
    if (has("HD") || has("hd")) return 90;        // aptX HD
    if (has("LC3")) return 85;                    // LE Audio
    if (has("aptX") || has("aptx")) return 70;    // plain aptX
    if (has("AAC")) return 65;
    if (has("XQ") || has("xq")) return 50;        // SBC-XQ
    return has("SBC") || has("sbc") ? 40 : 10;
}

// A Bluetooth output's battery from BlueZ (org.bluez.Battery1, needs "Experimental =
// true"); nullopt when the device exposes none.
constexpr auto kHeadsetName = "Arctis Nova Pro Wireless";

std::optional<int> btBatteryPercent(const std::string& nodeName) {
    const std::string prefix = "bluez_output.";
    const auto p = nodeName.find(prefix);
    if (p == std::string::npos) return std::nullopt;
    std::string mac = nodeName.substr(p + prefix.size());
    if (const auto dot = mac.find('.'); dot != std::string::npos) {
        mac.resize(dot);  // "80_C3_BA_94_FA_B8.1" -> "80_C3_BA_94_FA_B8"
    }
    // PipeWire names a BT sink with the MAC's colons both kept and underscored, and a
    // DBus object path allows only [A-Za-z0-9_] — normalise to cover both.
    for (char& c : mac) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0) {
            c = '_';
        }
    }
    if (mac.empty()) return std::nullopt;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return std::nullopt;
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
            if (pct >= 0 && pct <= 100) return pct;
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
    QVBoxLayout* root = makeScrollPage(this, 18);
    addPageHeader(root, QStringLiteral("Devices"),
                  QStringLiteral("Audio outputs on this system — USB, Bluetooth, HDMI and more"));

    // Live list of every connected output, driven by the PipeWire device registry.
    QVBoxLayout* outCard = makeCard(
        root, QStringLiteral("Connected outputs"),
        QStringLiteral("Everything the system can play to. Plug in or unplug a device and it "
                       "appears or disappears here."),
        10);
    deviceListBody_ = new QVBoxLayout;
    deviceListBody_->setSpacing(8);
    outCard->addLayout(deviceListBody_);

    // Per-device transmission format (ALSA outputs only; BT uses its codec).
    buildFormatCard(root);

    // SteelSeries base-station controls, shown only while it is connected.
    buildSteelSeriesControls(root);

    root->addStretch(1);

    hotplugTimer_ = new QTimer(this);
    hotplugTimer_->setSingleShot(true);
    hotplugTimer_->setInterval(3000);
    connect(hotplugTimer_, &QTimer::timeout, this, &DevicesPage::notifyDeviceChanges);

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
        ss, QStringLiteral("SteelSeries %1").arg(QString::fromUtf8(kHeadsetName)),
        QStringLiteral("USB base station · headset battery"),
        10);

    statusLabel_ = new QLabel(QStringLiteral("● Connected"));
    setState(statusLabel_, "ok");
    body->addWidget(statusLabel_);

    if (!probe.accessible) {
        // The same commands the Settings page's one-click fix runs, so the rule's path is
        // its real location for this install.
        QString text =
            QStringLiteral("No permission to talk to the base station. "
                           "Settings → System integration can install the udev rule "
                           "for you, or run it yourself:");
        const QString script = setup::privilegedFixScript(setup::CheckId::UdevRule);
        if (script.isEmpty()) {
            text += QStringLiteral("\n  (the rule file is missing from this build)");
        } else {
            for (const QString& line : script.split(QLatin1Char('\n'))) {
                // Drop the shell preamble: it belongs to the script that pkexec
                // runs, not to a line someone is meant to paste.
                if (line.isEmpty() || line.startsWith(QLatin1String("#!")) ||
                    line == QLatin1String("set -e")) {
                    continue;
                }
                text += QStringLiteral("\n  sudo ") + line;
            }
        }
        QLabel* hint = makeLabel(text, "Hint");
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        hint->setWordWrap(true);
        body->addWidget(hint);
    }
    body->addSpacing(6);

    // The base station reports the headset's battery only; the spare battery in
    // the dock is not part of its status report, so it is not shown here.
    batteryRow(body, QStringLiteral("Headset battery"), headsetBar_, headsetPct_);

    parent->addWidget(steelSeriesCard_);
}

void DevicesPage::notifyDeviceChanges() {
    if (notifier_ != nullptr) {
        // Keyed per device, so a reconnect replaces that device's disconnect.
        const auto deviceKey = [](const std::string& name) {
            return QStringLiteral("device:") + QString::fromStdString(name);
        };
        for (const auto& [name, desc] : pendingDevices_) {
            if (knownDevices_.find(name) == knownDevices_.end()) {
                notifier_->notify(QStringLiteral("Device connected"),
                                  QString::fromStdString(desc), Notifier::Low, QString(),
                                  /*clickable=*/false, deviceKey(name));
            }
        }
        for (const auto& [name, desc] : knownDevices_) {
            if (pendingDevices_.find(name) == pendingDevices_.end()) {
                notifier_->notify(QStringLiteral("Device disconnected"),
                                  QString::fromStdString(desc), Notifier::Low, QString(),
                                  /*clickable=*/false, deviceKey(name));
            }
        }
    }
    knownDevices_ = std::move(pendingDevices_);
    pendingDevices_.clear();
}

void DevicesPage::syncDevices() {
    if (devices_ != nullptr) {
        const std::uint64_t rev = devices_->devicesRevision();
        if (rev != lastDevicesRev_) {
            lastDevicesRev_ = rev;
            clearLayout(deviceListBody_);
            btBatteries_.clear();  // labels were just deleted with the rows
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
            // Notify on (dis)connects only once the list settles (HDMI outputs drop out on
            // every monitor sleep), and not on the first fill.
            {
                std::unordered_map<std::string, std::string> current;
                for (const audio::AudioDevice& d : list) {
                    current[d.name] = d.description;
                }
                if (devicesInitialized_) {
                    pendingDevices_ = std::move(current);
                    hotplugTimer_->start();  // restart: wait for the next quiet spell
                } else {
                    knownDevices_ = std::move(current);
                    devicesInitialized_ = true;
                }
            }
            if (list.empty()) {
                QLabel* none = makeLabel(QStringLiteral("No audio outputs detected."), "Hint");
                deviceListBody_->addWidget(none);
            } else {
                for (const audio::AudioDevice& dev : list) {
                    buildDeviceRow(deviceListBody_, dev);
                }
            }
            syncFormatDevices();  // keep the format selector in step with the list
        }
    }

    // Refresh Bluetooth battery levels in place (they drift over time), and
    // collect them for the tray.
    QList<BatteryLevel> levels;
    for (const BtBattery& bt : btBatteries_) {
        const std::optional<int> pct = updateBtBattery(bt.label, bt.nodeName);
        if (pct.has_value()) {
            levels.append({QString::fromStdString(bt.description), *pct, false});
        }
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
        refresh();  // fills headsetPercent_ / headsetCharging_
    } else {
        device_.close();
        headsetPercent_ = -1;
    }

    publishBatteries(std::move(levels));
}

void DevicesPage::publishBatteries(QList<BatteryLevel> levels) {
    // The headset leads: it is the one whose charge runs out mid-call.
    if (headsetPercent_ >= 0) {
        levels.prepend({QString::fromUtf8(kHeadsetName), headsetPercent_, headsetCharging_});
    }
    if (levels == lastBatteries_) return;  // nothing moved; leave any menu built from this alone
    lastBatteries_ = levels;
    emit batteriesChanged(levels);
}

void DevicesPage::buildDeviceRow(QVBoxLayout* body, const audio::AudioDevice& dev) {
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("DeviceRow"));
    row->setStyleSheet(QStringLiteral("#DeviceRow { background:%1; border-radius:10px; }")
            .arg(mixColors(currentTheme().surface, currentTheme().bg, 0.6).name(QColor::HexRgb)));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(12);

    const TypeStyle st = typeStyle(dev.type);
    auto* badge = new QLabel(QString::fromUtf8(st.label));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedWidth(64);
    // The badge text sits on the badge's own colour, so it cannot come from the
    // theme's text token — it has to contrast with whatever that colour is.
    const QColor badgeTint(st.color);
    badge->setStyleSheet(QStringLiteral("background:%1; color:%2; font-weight:800; "
                                        "font-size:11px; border-radius:7px; padding:5px 0;")
                             .arg(st.color,
                                  (badgeTint.lightness() > 140 ? currentTheme().bg
                                                               : currentTheme().text)
                                      .name(QColor::HexRgb)));

    auto* name = new QLabel(QString::fromStdString(dev.description));
    name->setStyleSheet(QStringLiteral("font-weight:700; font-size:13px;"));
    QLabel* sub = makeLabel(QString::fromStdString(dev.name), "Hint");
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
        pct->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                               .arg(mixColors(currentTheme().text, currentTheme().textDim, 0.6)
                                        .name(QColor::HexRgb)));
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
        // Switch to the best codec once per connection; manual changes are respected after.
        if (cc.valid && autoPreferredDevices_.insert(dev.name).second) {
            int bestRank = -1;
            std::uint32_t bestIdx = cc.currentIndex;
            for (const audio::DeviceCodec& c : cc.codecs) {
                if (!c.available) continue;
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
            QLabel* cap = makeLabel(QStringLiteral("Codec"), "Hint");
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
                QStringLiteral("background:%1; color:%2; font-weight:800; "
                               "font-size:9px; border-radius:5px; padding:3px 6px;")
                    .arg(currentTheme().warn.name(QColor::HexRgb),
                         // Dark ink on the warn colour, which is light by
                         // definition in any theme that means it as a warning.
                         currentTheme().bg.name(QColor::HexRgb)));
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
        btBatteries_.push_back({bat, dev.name, dev.description});
        h->addWidget(bat);
    }

    auto* dot = new QLabel(QStringLiteral("● live"));
    dot->setStyleSheet(QStringLiteral("color:%1; font-weight:700; font-size:11px;")
                           .arg(currentTheme().ok.name(QColor::HexRgb)));
    h->addWidget(dot);
    body->addWidget(row);
}

std::optional<int> DevicesPage::updateBtBattery(QLabel* label, const std::string& nodeName) {
    // BlueZ first (it speaks for Bluetooth nodes), then every generic source.
    std::optional<int> pct = btBatteryPercent(nodeName);
    if (!pct.has_value()) {
        if (const auto reading = hid::readBattery(batterySources_, nodeName)) {
            if (reading->valid()) {
                pct = reading->percent;
            }
        }
    }
    if (!pct.has_value()) {
        label->setVisible(false);
        return std::nullopt;
    }
    const int p = *pct;
    const ThemeTokens& theme = currentTheme();
    const QString color = (p <= 20 ? theme.danger : (p <= 40 ? theme.warn : theme.ok))
                              .name(QColor::HexRgb);
    label->setText(QStringLiteral("🔋 %1%").arg(p));
    label->setStyleSheet(
        QStringLiteral("color:%1; font-weight:700; font-size:12px;").arg(color));
    label->setVisible(true);

    const auto it = knownDevices_.find(nodeName);
    warnIfLow(nodeName, p,
              it != knownDevices_.end() ? QString::fromStdString(it->second)
                                        : QStringLiteral("Wireless device"));
    return pct;
}

void DevicesPage::warnIfLow(const std::string& key, int percent, const QString& what) {
    // Once per drain: warn at 20% or less, re-arm only above 25%, so a level
    // hovering around the threshold does not notify on every refresh.
    if (percent >= 0 && percent <= 20) {
        if (lowWarned_.insert(key).second && notifier_ != nullptr) {
            notifier_->notify(QStringLiteral("Battery low"),
                              QStringLiteral("%1 at %2%").arg(what).arg(percent),
                              Notifier::Critical, QStringLiteral("battery-caution"));
        }
    } else if (percent > 25) {
        lowWarned_.erase(key);
    }
}

void DevicesPage::buildFormatCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Transmission format"),
        QStringLiteral("Sample rate and bit depth an ALSA output's DAC is fed at. "
                       "Bluetooth outputs use their codec instead (set it on the device "
                       "above)."),
        10);

    formatDeviceCombo_ = new QComboBox;
    connect(formatDeviceCombo_, &QComboBox::activated, this,
            [this](int) { loadFormatForSelectedDevice(); });
    auto* devRow = new QHBoxLayout;
    devRow->setSpacing(8);
    devRow->addWidget(makeLabel(QStringLiteral("Device"), "CardKey"));
    devRow->addWidget(formatDeviceCombo_, 1);
    body->addLayout(devRow);

    rateCombo_ = new QComboBox;
    depthCombo_ = new QComboBox;

    auto* apply = makeButton(QStringLiteral("Save"));
    connect(apply, &QPushButton::clicked, this, &DevicesPage::applyFormat);

    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(makeLabel(QStringLiteral("Sample rate"), "CardKey"));
    row->addWidget(rateCombo_);
    row->addSpacing(16);
    row->addWidget(makeLabel(QStringLiteral("Bit depth"), "CardKey"));
    row->addWidget(depthCombo_);
    row->addStretch(1);
    row->addWidget(apply);
    body->addLayout(row);

    formatStatus_ = makeLabel(QString(), "Hint", true);
    body->addWidget(formatStatus_);

    syncFormatDevices();  // fill from whatever is already connected
}

void DevicesPage::syncFormatDevices() {
    if (formatDeviceCombo_ == nullptr || devices_ == nullptr) return;
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
    if (formatDeviceCombo_ == nullptr) return;
    const QString nodeName = formatDeviceCombo_->currentData().toString();
    if (nodeName.isEmpty()) return;
    const std::string name = nodeName.toStdString();

    // Offer only what the device supports: an unsupported rate makes the ALSA node fail
    // to open. The standard ladder only when the device could not be queried.
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
    if (formatDeviceCombo_ == nullptr) return;
    const QString nodeName = formatDeviceCombo_->currentData().toString();
    if (nodeName.isEmpty()) return;
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
        setState(formatStatus_, "bad");
    } else if (fmt.rateHz > 48000 && nodeName.contains(QStringLiteral("iec958"))) {
        // The card can send it, but most optical receivers take only 44.1 / 48 kHz and play
        // silence above that. Saved anyway, with a warning.
        formatStatus_->setText(
            QStringLiteral("Saved %1 for %2 — applies at next login. Careful: this is an "
                           "optical/S/PDIF output, and most speakers and receivers on one "
                           "accept only 44.1 or 48 kHz. If it goes silent after the next "
                           "login, set 48 kHz here.")
                .arg(what, formatDeviceCombo_->currentText()));
        setState(formatStatus_, "bad");
    } else {
        formatStatus_->setText(
            QStringLiteral("Saved %1 for %2 — applies at next login (or `systemctl --user "
                           "restart wireplumber`). Bluetooth is not disturbed.")
                .arg(what, formatDeviceCombo_->currentText()));
        setState(formatStatus_, "ok");
    }
}

void DevicesPage::refresh() {
    const hid::BatteryStatus s = device_.readBattery();
    if (!s.valid) {
        statusLabel_->setText(QStringLiteral("● No response"));
        setState(statusLabel_, "bad");
        headsetPercent_ = -1;
        return;
    }
    // The dock stays reachable even when the headset itself is powered off, so
    // report the headset's link state rather than just "connected".
    switch (s.state) {
        case hid::HeadsetState::Offline:
            statusLabel_->setText(QStringLiteral("● Headset off"));
            setState(statusLabel_, "warn");
            break;
        case hid::HeadsetState::Charging:
            statusLabel_->setText(QStringLiteral("● Charging"));
            setState(statusLabel_, "ok");
            break;
        default:
            statusLabel_->setText(QStringLiteral("● Connected"));
            setState(statusLabel_, "ok");
            break;
    }

    // A powered-off headset reports no meaningful charge, so it drops out of the
    // tray rather than showing a stale number.
    headsetCharging_ = s.state == hid::HeadsetState::Charging;
    headsetPercent_ = s.state == hid::HeadsetState::Offline ? -1 : s.headsetPercent;

    headsetBar_->setValue(std::max(0, s.headsetPercent));
    headsetPct_->setText(s.headsetPercent < 0 ? QStringLiteral("—")
                                              : QStringLiteral("%1%").arg(s.headsetPercent));

    // Not a PipeWire node name, so it cannot collide with a Bluetooth device's.
    warnIfLow("hid:headset", s.headsetPercent, QStringLiteral("Headset"));
}

}  // namespace sonar::ui
