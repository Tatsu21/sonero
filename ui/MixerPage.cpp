#include "ui/MixerPage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
#include "audio/IAudioDevices.h"
#include "audio/IChannelController.h"
#include "audio/IMixer.h"
#include "config/SettingsStore.h"
#include "core/Log.h"
#include "ui/widgets/AppChip.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/FlowLayout.h"

namespace sonar::ui {

using audio::ChannelId;
using audio::ChannelLevel;

namespace {
constexpr int kRefreshIntervalMs = 40;  // ~25 fps meter updates

// The channel that acts as the master fader for all the others.
constexpr ChannelId kMasterChannel = ChannelId::System;

QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

QString channelKey(ChannelId id) { return toQString(audio::channelName(id)); }

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
}
}  // namespace

MixerPage::MixerPage(audio::IMixer& mixer, audio::IAppRouter* router,
                     audio::IChannelController* controller, audio::IAudioDevices* devices,
                     config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent),
      mixer_(mixer),
      router_(router),
      controller_(controller),
      devices_(devices),
      settings_(settings) {
    // Real metering is available only through the controller; otherwise fall
    // back to the placeholder simulator so the meters are not simply dead.
    simulate_ = (controller_ == nullptr);

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

    auto* title = new QLabel(QStringLiteral("Mixer"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("Route apps to channels and shape each one"), this);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* titleCol = new QVBoxLayout;
    titleCol->setSpacing(4);
    titleCol->addWidget(title);
    titleCol->addWidget(subtitle);

    // Stream mode reveals the second mix: a "to stream" fader per channel, feeding
    // the Sonero Stream source a capture application (OBS, Discord) records. What
    // you hear and what the stream hears become independent.
    auto* streamCheck = new QCheckBox(QStringLiteral("Stream mode"), this);
    streamCheck->setToolTip(QStringLiteral(
        "Show a second fader per channel controlling how loud it is for the "
        "stream.\nIn OBS, pick \"Monitor of Sonero Stream\" as the audio source."));
    connect(streamCheck, &QCheckBox::toggled, this, [this](bool on) {
        streamMode_ = on;
        for (auto* strip : strips_) {
            strip->setStreamMode(on);
        }
        if (settings_ != nullptr) {
            QJsonObject g = settings_->section(QStringLiteral("general"));
            g[QStringLiteral("streamMode")] = on;
            settings_->putSection(QStringLiteral("general"), g);
        }
    });

    auto* header = new QHBoxLayout;
    header->addLayout(titleCol);
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
        auto* strip = new ChannelStrip(id, toQString(audio::channelName(id)), this);
        strip->setState(mixer_.state(id));

        connect(strip, &ChannelStrip::volumeChanged, this, [this](ChannelId ch, float v) {
            mixer_.setVolume(ch, v);
            if (controller_ != nullptr) {
                // Moving the master rescales every channel, so re-push them all.
                if (ch == kMasterChannel) {
                    pushChannelVolumes();
                } else {
                    controller_->setChannelVolume(
                        ch, v * mixer_.state(kMasterChannel).volume);
                }
            }
            saveMixerState();
        });
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
                    if (router_ == nullptr) {
                        return;
                    }
                    // Defer past the drag's nested event loop so the app-chip
                    // rebuild (triggered by the routing change) never deletes the
                    // chip that is still inside QDrag::exec().
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
        stripRow->addWidget(strip);
    }
    stripRow->addStretch(1);
    root->addLayout(stripRow, 1);

    if (settings_ != nullptr) {
        streamMode_ = settings_->section(QStringLiteral("general"))
                          .value(QStringLiteral("streamMode"))
                          .toBool(false);
        streamCheck->setChecked(streamMode_);
    }  // the strips get the room, not the pool below

    // Push the model's initial state onto the real channel sinks. Some sinks may
    // not be bound yet, and the session manager applies its own default volume
    // shortly after each sink appears, so re-assert once things have settled.
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

    auto* appsTitle = new QLabel(QStringLiteral("Unrouted applications"), appsSection_);
    appsTitle->setObjectName(QStringLiteral("SectionTitle"));
    sectionCol->addWidget(appsTitle);

    if (router_ != nullptr) {
        auto* dragHint = new QLabel(
            QStringLiteral("Drag an app onto a channel above to route it."), this);
        dragHint->setObjectName(QStringLiteral("Hint"));
        sectionCol->addWidget(dragHint);

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
        auto* hint = new QLabel(
            QStringLiteral("Routing requires a running PipeWire server (unavailable)."),
            this);
        hint->setObjectName(QStringLiteral("Hint"));
        sectionCol->addWidget(hint);
        root->addStretch(1);
    }

    timer_ = new QTimer(this);
    timer_->setInterval(kRefreshIntervalMs);
    connect(timer_, &QTimer::timeout, this, &MixerPage::refresh);
    timer_->start();

    log::debug("MixerPage: initialized ({} channels, routing {}, metering {})",
               strips_.size(), router_ != nullptr ? "on" : "off",
               controller_ != nullptr ? "real" : "simulated");
}
void MixerPage::syncOutputDevices() {
    // The output selector now lives on the Channels page; nothing to refresh here.
}

void MixerPage::pushChannelBalances() {
    if (controller_ == nullptr) {
        return;
    }
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        controller_->setChannelBalance(id, mixer_.state(id).balance);
    }
}

void MixerPage::pushChannelVolumes() {
    if (controller_ == nullptr) {
        return;
    }
    // System is the master: its fader scales every other channel, so pulling it
    // down brings the whole mix with it. Applied as a multiplier here rather than
    // in the model, so each channel's own fader keeps showing what the user set.
    const float master = mixer_.state(kMasterChannel).volume;
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        const float own = mixer_.state(id).volume;
        controller_->setChannelVolume(id, id == kMasterChannel ? own : own * master);
    }
}

void MixerPage::pushMuteStates() {
    if (controller_ == nullptr) {
        return;
    }
    // Mute + solo together decide audibility; the master mute silences everything
    // on top of that.
    const bool masterSilent = !mixer_.isAudible(kMasterChannel);
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) continue;
        controller_->setChannelMute(id, !mixer_.isAudible(id) || masterSilent);
    }
}

void MixerPage::applyChannelGainDb(ChannelId id, float gainDb) {
    channelGainDb_[static_cast<int>(id)] = gainDb;
    if (controller_ != nullptr) {
        controller_->setChannelGain(id, std::pow(10.0f, gainDb / 20.0f));
    }
    saveMixerState();
}

void MixerPage::applyChannelAutoGain(ChannelId id, bool on) {
    autoGainOn_[static_cast<int>(id)] = on;
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

float MixerPage::channelGainDb(ChannelId id) const {
    const auto it = channelGainDb_.find(static_cast<int>(id));
    return it != channelGainDb_.end() ? it->second : 0.0f;
}

bool MixerPage::channelAutoGain(ChannelId id) const {
    const auto it = autoGainOn_.find(static_cast<int>(id));
    return it != autoGainOn_.end() && it->second;
}

void MixerPage::saveAppRouting(const QString& appName, audio::ChannelId channel) {
    if (settings_ == nullptr || appName.isEmpty()) {
        return;
    }
    savedAppChannel_[appName] = static_cast<int>(channel);
    QJsonObject apps = settings_->section(QStringLiteral("appRouting"));
    apps[appName] = channelKey(channel);
    settings_->putSection(QStringLiteral("appRouting"), apps);
}

void MixerPage::restoreAppRouting() {
    if (router_ == nullptr || savedAppChannel_.empty()) {
        return;
    }
    for (const audio::AppStream& app : router_->applications()) {
        // Only place a stream once: after that the user is free to move it, and
        // re-asserting on every refresh would drag it back.
        if (routedAppIds_.count(app.id) != 0) {
            continue;
        }
        const auto it = savedAppChannel_.find(QString::fromStdString(app.name));
        if (it == savedAppChannel_.end()) {
            continue;
        }
        const auto channel = static_cast<ChannelId>(it->second);
        routedAppIds_.insert(app.id);
        if (app.channel != channel) {
            router_->assign(app.id, channel);
            log::debug("Mixer: restored '{}' to {}", app.name,
                       std::string(audio::channelName(channel)));
        }
    }
    // Forget ids that have gone away, so a restarted application is placed again.
    for (auto it = routedAppIds_.begin(); it != routedAppIds_.end();) {
        bool alive = false;
        for (const audio::AppStream& app : router_->applications()) {
            if (app.id == *it) { alive = true; break; }
        }
        it = alive ? std::next(it) : routedAppIds_.erase(it);
    }
}

void MixerPage::updateAutoGain(ChannelStrip* strip, float peak) {
    const int key = static_cast<int>(strip->channelId());
    if (!autoGainOn_[key] || controller_ == nullptr) {
        return;
    }
    // The meter reads the channel output, which already carries the EQ and filter
    // headroom. Divide that back out so the loop judges how hot the *source* is —
    // otherwise its target sits below anything the chain can produce and it never
    // has a reason to act.
    const float headroom = std::max(controller_->channelHeadroom(strip->channelId()), 0.01f);
    const float dt = static_cast<float>(kRefreshIntervalMs) / 1000.0f;
    const float gainDb = autoGain_[key].update(peak / headroom, dt);

    // Only touch the audio path when the value actually moved: the loop runs 25
    // times a second and most ticks change nothing.
    if (std::abs(gainDb - channelGainDb_[key]) < 0.05f) {
        return;
    }
    channelGainDb_[key] = gainDb;
    controller_->setChannelGain(strip->channelId(), std::pow(10.0f, gainDb / 20.0f));
}

void MixerPage::restoreMixerState() {
    if (settings_ == nullptr) {
        return;
    }
    const QJsonObject m = settings_->section(QStringLiteral("mixer"));
    if (m.isEmpty()) {
        return;  // first run — keep the model's built-in defaults
    }
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // the mic is owned by the Microphone page's own section
        }
        const QJsonObject c = m.value(channelKey(id)).toObject();
        if (c.isEmpty()) {
            continue;
        }
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
        if (controller_ != nullptr && gainDb != 0.0f) {
            controller_->setChannelGain(id, std::pow(10.0f, gainDb / 20.0f));
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
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject m;
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // persisted by the Microphone page, not here
        }
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
    if (simulate_ || controller_ == nullptr) {
       // injectSimulatedLevels();
        for (auto* strip : strips_) {
            const ChannelLevel lvl = mixer_.level(strip->channelId());
            strip->setLevel(lvl.peakLeft, lvl.peakRight);
        }
    } else {
        static const bool debugLevels = std::getenv("SONAR_DEBUG_LEVELS") != nullptr;
        static int debugTick = 0;
        float maxPeak = 0.0f;
        for (auto* strip : strips_) {
            const ChannelLevel lvl = controller_->channelLevel(strip->channelId());
            strip->setLevel(lvl.peakLeft, lvl.peakRight);
            maxPeak = std::max({maxPeak, lvl.peakLeft, lvl.peakRight});
            updateAutoGain(strip, std::max(lvl.peakLeft, lvl.peakRight));
        }
        if (debugLevels && (++debugTick % 25 == 0)) {
            log::debug("MixerPage: max channel peak = {:.5f}", maxPeak);
        }
    }

    // Refresh the per-channel output selectors when devices come or go.
    if (devices_ != nullptr) {
        const std::uint64_t drev = devices_->devicesRevision();
        if (drev != devicesRevision_) {
            devicesRevision_ = drev;
            syncOutputDevices();
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

    // Each chip now lives inside the channel it is routed to, so a column shows
    // what it is actually carrying. Only genuinely unrouted streams stay in the
    // pool below.
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

void MixerPage::injectSimulatedLevels() {
    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // mic lives on its own page, not in the output mixer
        }
        if (!mixer_.isAudible(id)) {
            mixer_.setLevel(id, ChannelLevel{0.0f, 0.0f});
            continue;
        }
        const auto state = mixer_.state(id);
        const float n = noise_(rng_);
        const float peak = state.volume * (0.35f + 0.6f * n * n);

        const float leftGain = 1.0f - std::max(state.balance, 0.0f) * 0.7f;
        const float rightGain = 1.0f - std::max(-state.balance, 0.0f) * 0.7f;
        mixer_.setLevel(id, ChannelLevel{peak * leftGain, peak * rightGain});
    }
}

}  // namespace sonar::ui
