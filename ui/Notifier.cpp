#include "ui/Notifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

#include "config/SettingsStore.h"

namespace sonar::ui {

namespace {
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
}  // namespace

Notifier::Notifier(config::SettingsStore* settings, QObject* parent)
    : QObject(parent), settings_(settings) {
    if (settings_ != nullptr) {
        enabled_ = settings_->section(QStringLiteral("general"))
                       .value(QStringLiteral("notifications"))
                       .toBool(true);
    }
    // Route click / dismissal callbacks from the notification service back to us.
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, QStringLiteral("ActionInvoked"), this,
                SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kService, QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(uint, uint)));
}

void Notifier::setEnabled(bool on) {
    enabled_ = on;
    if (settings_ != nullptr) {
        QJsonObject g = settings_->section(QStringLiteral("general"));
        g[QStringLiteral("notifications")] = on;
        settings_->putSection(QStringLiteral("general"), g);
    }
}

void Notifier::notify(const QString& title, const QString& body, Urgency urgency,
                      const QString& icon, bool clickable) {
    if (!enabled_) {
        return;
    }
    QDBusMessage msg =
        QDBusMessage::createMethodCall(kService, kPath, kService, QStringLiteral("Notify"));

    QVariantMap hints;
    hints[QStringLiteral("urgency")] = QVariant::fromValue(static_cast<uchar>(urgency));
    hints[QStringLiteral("desktop-entry")] = QStringLiteral("Sonero");

    // A "default" action is the one most servers (GNOME Shell, KDE) invoke when the
    // notification body itself is clicked.
    QStringList actions;
    if (clickable) {
        actions << QStringLiteral("default") << QStringLiteral("Open");
    }

    msg << QStringLiteral("Sonero")                                          // app_name
        << 0U                                                                    // replaces_id
        << (icon.isEmpty() ? QStringLiteral("audio-headphones") : icon)          // app_icon
        << title                                                                 // summary
        << body                                                                  // body
        << actions                                                               // actions
        << hints                                                                 // hints
        << -1;                                                                   // expire_timeout

    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg);
    if (!clickable) {
        return;  // fire-and-forget; no need to track the returned id
    }
    // Remember the notification id so we can match its ActionInvoked callback.
    auto* watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* w) {
                const QDBusPendingReply<uint> reply = *w;
                if (reply.isValid()) {
                    clickableIds_.insert(reply.value());
                }
                w->deleteLater();
            });
}

void Notifier::onActionInvoked(uint id, const QString& actionKey) {
    Q_UNUSED(actionKey);
    if (clickableIds_.remove(id)) {
        emit activated();
    }
}

void Notifier::onNotificationClosed(uint id, uint reason) {
    Q_UNUSED(reason);
    clickableIds_.remove(id);  // stop tracking a notification that is gone
}

}  // namespace sonar::ui
