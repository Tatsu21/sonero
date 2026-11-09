#include "ui/MixerPage.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QJsonObject>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "audio/IAppRouter.h"
#include "audio/IChannelController.h"
#include "audio/IMixer.h"
#include "config/SettingsStore.h"
#include "core/Log.h"
#include "ui/UiKit.h"
#include "ui/widgets/AppChip.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/FlowLayout.h"

namespace sonar::ui {

using audio::ChannelId;
using audio::ChannelLevel;

namespace {
constexpr int kRefreshIntervalMs = 40;  // ~25 fps meter updates
// Hidden, the refresh only places newly started apps on their remembered channel.
constexpr int kHiddenRefreshMs = 250;

// The channel that acts as the master fader for all the others.
constexpr ChannelId kMasterChannel = ChannelId::System;

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

MixerPage::MixerPage(audio::IMixer& mixer, audio::IAppRouter* router,
                     audio::IChannelController* controller, config::SettingsStore* settings,
                     QWidget* parent)
    : QWidget(parent),
      mixer_(mixer),
      router_(router),
      controller_(controller),
      settings_(settings) {
    // Load any persisted channel state before the strips read the model.
    restoreMixerState();
    if (settings_ != nullptr) {
        const QJsonObject apps = settings_->section(QStringLiteral("appRouting"));
        for (auto it = apps.begin(); it != apps.end(); ++it) {
            for (const ChannelId id : audio::kAllChannels) {
                if (channelKey(id) == it.value().toString()) {
                    savedAppChannel_[it.key()] = static_cast<int>(id);
                    break;
                }
            }
        }
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 28);
    root->setSpacing(18);

    // Stream mode: a second "to stream" fader per channel, feeding the Sonero Stream
    // source independently of what you hear.
    auto* streamCheck = new QCheckBox(QStringLiteral("Stream mode"), this);
    streamCheck->setToolTip(QStringLiteral(
        "Show a second fader per channel controlling how loud it is for the "
        "stream.\nIn OBS, pick \"Monitor of Sonero Stream\" as the audio source."));
    connect(streamCheck, &QCheckBox::toggled, this, [this](bool on) {
        for (auto* strip : strips_) {
            strip->setStreamMode(on);
        }
        if (settings_ != nullptr) {
            settings_->putValue(QStringLiteral("general"), QStringLiteral("streamMode"), on);
        }
    });

    auto* header = new QHBoxLayout;
    addPageHeader(header, QStringLiteral("Mixer"),
                  QStringLiteral("Route apps to channels and shape each one"));
    header->addStretch(1);
    header->addWidget(streamCheck, 0, Qt::AlignTop);
    root->addLayout(header);

    auto* stripRow = new QHBoxLayout;
    stripRow->setSpacing(10);
    stripRow->setAlignment(Qt::AlignLeft);

    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // the mic is a source, controlled on the Microphone page
        }
        auto* strip = new ChannelStrip(id, channelKey(id), this);
        strip->setState(mixer_.state(id));

        connect(strip, &ChannelStrip::volumeChanged, this, &MixerPage::applyChannelVolume);
        strip->setStreamLevel(streamLevel_.count(static_cast<int>(id)) != 0
                                  ? streamLevel_[static_cast<int>(id)]
                                  : 1.0f);
        connect(strip, &ChannelStrip::streamLevelChanged, this,
                [this](ChannelId ch, float level) {
                    streamLevel_[static_cast<int>(ch)] = level;
                    if (controller_ != nullptr) {
                        controller_->setStreamLevel(ch, level);
                    }
                    saveMixerState();
                });
        connect(strip, &ChannelStrip::balanceChanged, this, [this](ChannelId ch, float b) {
            mixer_.setBalance(ch, b);
            if (controller_ != nullptr) {
                controller_->setChannelBalance(ch, b);
            }
            saveMixerState();
        });
        connect(strip, &ChannelStrip::muteToggled, this, [this](ChannelId ch, bool m) {
            mixer_.setMuted(ch, m);
            pushMuteStates();
            saveMixerState();
        });
        connect(strip, &ChannelStrip::soloToggled, this, [this](ChannelId ch, bool s) {
            mixer_.setSolo(ch, s);
            pushMuteStates();
            saveMixerState();
        });
        connect(strip, &ChannelStrip::appDropped, this,
                [this](ChannelId ch, std::uint32_t appId) {
                    if (router_ == nullptr) return;
                    // Defer past QDrag::exec(), so the chip rebuild never deletes
                    // the chip being dragged.
                    QTimer::singleShot(0, this, [this, ch, appId] {
                        router_->assign(appId, ch);
                        routedAppIds_.insert(appId);
                        // Remember by name: the id is gone the next time the
                        // application starts.
                        for (const audio::AppStream& app : router_->applications()) {
                            if (app.id == appId) {
                                saveAppRouting(QString::fromStdString(app.name), ch);
                                break;
                            }
                        }
                    });
                });

        strips_.push_back(strip);
        stripByChannel_[static_cast<int>(id)] = strip;
        stripRow->addWidget(strip, 1);  // share the row; the strip caps its own width
    }
    // No trailing stretch: the strips themselves take the space now, and a
    // stretch here would hand it all back to the right-hand margin.

    root->addLayout(stripRow, 1);

    if (settings_ != nullptr) {
        streamCheck->setChecked(
            settings_->value(QStringLiteral("general"), QStringLiteral("streamMode"))
                .toBool(false));
    }

    // Push the model onto the sinks, and again once they have settled: the session
    // manager applies its own default volume after each appears.
    if (controller_ != nullptr) {
        pushChannelVolumes();
        pushMuteStates();
        pushChannelBalances();
        QTimer::singleShot(400, this, [this] { pushChannelVolumes(); pushMuteStates(); });
        QTimer::singleShot(1500, this, [this] { pushChannelVolumes(); pushMuteStates(); });
    }

    // --- Applications panel --------------------------------------------------
    appsSection_ = new QWidget(this);
    auto* sectionCol = new QVBoxLayout(appsSection_);
    sectionCol->setContentsMargins(0, 0, 0, 0);
    sectionCol->setSpacing(8);
    root->addWidget(appsSection_);

    auto* separator = new QFrame(appsSection_);
    separator->setObjectName(QStringLiteral("Sep"));
    separator->setFrameShape(QFrame::NoFrame);
    separator->setFixedHeight(1);
    sectionCol->addWidget(separator);

    sectionCol->addWidget(makeLabel(QStringLiteral("Unrouted applications"), "SectionTitle"));

    if (router_ != nullptr) {
        sectionCol->addWidget(
            makeLabel(QStringLiteral("Drag an app onto a channel above to route it."), "Hint"));

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMinimumHeight(96);

        auto* host = new QWidget;
        appsLayout_ = new FlowLayout(host, 0, 10, 10);
        scroll->setWidget(host);
        scroll->setMaximumHeight(110);
        sectionCol->addWidget(scroll);
    } else {
        sectionCol->addWidget(makeLabel(
            QStringLiteral("Routing requires a running PipeWire server (unavailable)."), "Hint"));
        root->addStretch(1);
    }

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MixerPage::refresh);
    updateRefreshRate();
    timer_->start();

    log::debug("MixerPage: initialized ({} channels, routing {}, metering {})",
               strips_.size(), router_ != nullptr ? "on" : "off",
               controller_ != nullptr ? "real" : "none");
}

void MixerPage::pushChannelBalances() {
    if (controller_ == nullptr) return;
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        controller_->setChannelBalance(id, mixer_.state(id).balance);
    }
}

void MixerPage::pushChannelVolumes() {
    if (controller_ == nullptr) return;
    // System is the master: its fader multiplies every other channel, applied here so
    // each fader still shows what the user set.
    const float master = mixer_.state(kMasterChannel).volume;
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        const float own = mixer_.state(id).volume;
        controller_->setChannelVolume(id, id == kMasterChannel ? own : own * master);
    }
}

void MixerPage::pushMuteStates() {
    if (controller_ == nullptr) return;
    // Mute + solo together decide audibility; the master mute silences everything
    // on top of that.
    const bool masterSilent = !mixer_.isAudible(kMasterChannel);
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        controller_->setChannelMute(id, !mixer_.isAudible(id) || masterSilent);
    }
}

void MixerPage::applyChannelVolume(ChannelId id, float volume) {
    mixer_.setVolume(id, volume);
    if (controller_ != nullptr) {
        // Moving the master rescales every channel, so re-push them all.
        if (id == kMasterChannel) {
            pushChannelVolumes();
        } else {
            controller_->setChannelVolume(id, volume * mixer_.state(kMasterChannel).volume);
        }
    }
    saveMixerState();

    // Report what the model actually holds, not what was asked for: setVolume
    // clamps. setState blocks the strip's signals, so this cannot echo back.
    const float stored = mixer_.state(id).volume;
    if (const auto it = stripByChannel_.find(static_cast<int>(id));
        it != stripByChannel_.end() && it->second != nullptr) {
        it->second->setState(mixer_.state(id));
    }
    emit channelVolumeChanged(id, stored);
}

void MixerPage::pushChannelGain(ChannelId id, float gainDb) {
    if (controller_ != nullptr) {
        controller_->setChannelGain(id, dbToLinear(gainDb));
    }
}

void MixerPage::applyChannelGainDb(ChannelId id, float gainDb) {
    channelGainDb_[static_cast<int>(id)] = gainDb;
    pushChannelGain(id, gainDb);
    saveMixerState();
}

void MixerPage::updateRefreshRate() {
    if (timer_ == nullptr) return;
    // Auto-gain steps on every meter reading, shown or not.
    const bool autoGain = std::any_of(autoGainOn_.begin(), autoGainOn_.end(),
                                      [](const auto& entry) { return entry.second; });
    timer_->setInterval(isVisible() || autoGain ? kRefreshIntervalMs : kHiddenRefreshMs);
}

void MixerPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateRefreshRate();
}

void MixerPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    updateRefreshRate();
}

void MixerPage::applyChannelAutoGain(ChannelId id, bool on) {
    autoGainOn_[static_cast<int>(id)] = on;
    updateRefreshRate();
    if (on) {
        // Start from what the user had dialled in, so enabling it does not jump.
        autoGain_[static_cast<int>(id)].reset(channelGainDb_[static_cast<int>(id)]);
    }
    saveMixerState();
}

void MixerPage::applyChannelOutput(ChannelId id, const QString& deviceNodeName) {
    if (controller_ != nullptr) {
        controller_->setChannelOutput(id, deviceNodeName.toStdString());
    }
    saveMixerState();
}

QString MixerPage::channelOutput(ChannelId id) const {
    if (controller_ == nullptr) return {};
    return QString::fromStdString(controller_->channelOutput(id));
}

float MixerPage::channelGainDb(ChannelId id) const {
    const auto it = channelGainDb_.find(static_cast<int>(id));
    return it != channelGainDb_.end() ? it->second : 0.0f;
}

bool MixerPage::channelAutoGain(ChannelId id) const {
    const auto it = autoGainOn_.find(static_cast<int>(id));
    return it != autoGainOn_.end() && it->second;
}

void MixerPage::saveAppRouting(const QString& appName, audio::ChannelId channel) {
    if (settings_ == nullptr || appName.isEmpty()) return;
    savedAppChannel_[appName] = static_cast<int>(channel);
    QJsonObject apps = settings_->section(QStringLiteral("appRouting"));
    apps[appName] = channelKey(channel);
    settings_->putSection(QStringLiteral("appRouting"), apps);
}

void MixerPage::restoreAppRouting() {
    if (router_ == nullptr || savedAppChannel_.empty()) return;
    for (const audio::AppStream& app : router_->applications()) {
        // Only place a stream once: after that the user is free to move it, and
        // re-asserting on every refresh would drag it back.
        if (routedAppIds_.count(app.id) != 0) continue;
        const auto it = savedAppChannel_.find(QString::fromStdString(app.name));
        if (it == savedAppChannel_.end()) continue;
        const auto channel = static_cast<ChannelId>(it->second);
        routedAppIds_.insert(app.id);
        if (app.channel != channel) {
            router_->assign(app.id, channel);
            log::debug("Mixer: restored '{}' to {}", app.name,
                       std::string(audio::channelName(channel)));
        }
    }
    // Forget ids that have gone away, so a restarted application is placed again.
    std::set<std::uint32_t> alive;
    for (const audio::AppStream& app : router_->applications()) {
        alive.insert(app.id);
    }
    for (auto it = routedAppIds_.begin(); it != routedAppIds_.end();) {
        it = alive.count(*it) != 0 ? std::next(it) : routedAppIds_.erase(it);
    }
}

void MixerPage::updateAutoGain(ChannelStrip* strip, float peak) {
    const int key = static_cast<int>(strip->channelId());
    if (!autoGainOn_[key] || controller_ == nullptr) return;
    // The meter reads the channel's output (post-EQ, post-fader) — the signal that
    // reaches the device. A lower fader really does leave less to protect.
    const ChannelId id = strip->channelId();
    const float dt = static_cast<float>(kRefreshIntervalMs) / 1000.0f;
    const float gainDb = autoGain_[key].update(peak, dt);

    // Only touch the audio path when the value actually moved: the loop runs 25
    // times a second and most ticks change nothing.
    if (std::abs(gainDb - channelGainDb_[key]) < 0.05f) return;
    channelGainDb_[key] = gainDb;
    pushChannelGain(id, gainDb);
    emit channelGainDbChanged(id, gainDb);
}

void MixerPage::restoreMixerState() {
    if (settings_ == nullptr) return;
    const QJsonObject m = settings_->section(QStringLiteral("mixer"));
    if (m.isEmpty()) return;  // first run — keep the model's built-in defaults
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // the mic is owned by the Microphone page's own section
        }
        const QJsonObject c = m.value(channelKey(id)).toObject();
        if (c.isEmpty()) continue;
        const audio::ChannelState cur = mixer_.state(id);
        mixer_.setVolume(
            id, static_cast<float>(c.value(QStringLiteral("volume")).toDouble(cur.volume)));
        mixer_.setBalance(
            id, static_cast<float>(c.value(QStringLiteral("balance")).toDouble(cur.balance)));
        mixer_.setMuted(id, c.value(QStringLiteral("muted")).toBool(cur.muted));
        mixer_.setSolo(id, c.value(QStringLiteral("solo")).toBool(cur.solo));
        const float gainDb =
            static_cast<float>(c.value(QStringLiteral("gainDb")).toDouble(0.0));
        channelGainDb_[static_cast<int>(id)] = gainDb;
        autoGainOn_[static_cast<int>(id)] =
            c.value(QStringLiteral("autoGain")).toBool(false);
        const float sendLevel =
            static_cast<float>(c.value(QStringLiteral("streamLevel")).toDouble(1.0));
        streamLevel_[static_cast<int>(id)] = sendLevel;
        if (controller_ != nullptr) {
            controller_->setStreamLevel(id, sendLevel);
        }
        autoGain_[static_cast<int>(id)].reset(gainDb);
        if (gainDb != 0.0f) {
            pushChannelGain(id, gainDb);
        }
        if (controller_ != nullptr && c.contains(QStringLiteral("output"))) {
            controller_->setChannelOutput(
                id, c.value(QStringLiteral("output")).toString().toStdString());
        }
    }
    // Mirror the restored model onto the real audio path.
    pushChannelVolumes();
    pushChannelBalances();
    pushMuteStates();
}

void MixerPage::saveMixerState() {
    if (settings_ == nullptr) return;
    QJsonObject m;
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;  // persisted by the Microphone page, not here
        const audio::ChannelState st = mixer_.state(id);
        QJsonObject c;
        c[QStringLiteral("volume")] = st.volume;
        c[QStringLiteral("balance")] = st.balance;
        c[QStringLiteral("muted")] = st.muted;
        if (const auto it = streamLevel_.find(static_cast<int>(id)); it != streamLevel_.end()) {
            c[QStringLiteral("streamLevel")] = it->second;
        }
        if (const auto it = channelGainDb_.find(static_cast<int>(id));
            it != channelGainDb_.end()) {
            c[QStringLiteral("gainDb")] = it->second;
        }
        if (const auto it = autoGainOn_.find(static_cast<int>(id)); it != autoGainOn_.end()) {
            c[QStringLiteral("autoGain")] = it->second;
        }
        c[QStringLiteral("solo")] = st.solo;
        if (controller_ != nullptr) {
            c[QStringLiteral("output")] =
                QString::fromStdString(controller_->channelOutput(id));
        }
        m[channelKey(id)] = c;
    }
    settings_->putSection(QStringLiteral("mixer"), m);
}

void MixerPage::refresh() {
    if (controller_ == nullptr) {
        for (auto* strip : strips_) {
            const ChannelLevel lvl = mixer_.level(strip->channelId());
            strip->setLevel(lvl.peakLeft, lvl.peakRight);
        }
    } else {
        for (auto* strip : strips_) {
            const ChannelLevel lvl = controller_->channelLevel(strip->channelId());
            strip->setLevel(lvl.peakLeft, lvl.peakRight);
            updateAutoGain(strip, std::max(lvl.peakLeft, lvl.peakRight));
        }
    }

    // Never rebuild the chips while a drag is in flight — deleting the dragged
    // chip inside QDrag::exec() would crash.
    if (router_ != nullptr && AppChip::draggedAppId() == 0) {
        const std::uint64_t rev = router_->revision();
        if (rev != appsRevision_) {
            appsRevision_ = rev;
            restoreAppRouting();  // place newly-appeared apps before drawing them
            rebuildApplications();
        }
    }
}

void MixerPage::rebuildApplications() {
    const std::vector<audio::AppStream> apps = router_->applications();

    // Routed chips live in their channel's column; only unrouted streams stay in the pool.
    for (auto* strip : strips_) {
        strip->clearApps();
    }
    if (appsLayout_ != nullptr) {
        clearLayout(appsLayout_);
    }

    int unrouted = 0;
    for (const audio::AppStream& app : apps) {
        auto* chip = new AppChip(app.id, QString::fromStdString(app.name), app.channel);
        if (app.channel) {
            const auto it = stripByChannel_.find(static_cast<int>(*app.channel));
            if (it != stripByChannel_.end()) {
                it->second->addApp(chip);
                continue;
            }
        }
        if (appsLayout_ != nullptr) {
            appsLayout_->addWidget(chip);
            ++unrouted;
        } else {
            chip->deleteLater();
        }
    }

    if (appsSection_ != nullptr) {
        appsSection_->setVisible(unrouted > 0);
    }
}

}  // namespace sonar::ui
