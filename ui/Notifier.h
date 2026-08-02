#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

// Sends desktop notifications via the freedesktop D-Bus service
// (org.freedesktop.Notifications) — natively handled by GNOME Shell, KDE, etc.
// The "show notifications" preference is persisted in the settings store.
//
// Notifications can optionally carry a "default" action so that clicking the
// notification does something: the service reports the click back over the
// ActionInvoked D-Bus signal, which we surface as the activated() signal.
class Notifier : public QObject {
    Q_OBJECT

public:
    enum Urgency { Low = 0, Normal = 1, Critical = 2 };

    explicit Notifier(config::SettingsStore* settings, QObject* parent = nullptr);

    [[nodiscard]] bool enabled() const { return enabled_; }
    void setEnabled(bool on);  // persists to the settings store

    // Fire-and-forget a notification (no-op when disabled). `icon` is a themed
    // icon name; a sensible default is used when empty. When `clickable` is true
    // the notification gets a default action and clicking it emits activated().
    void notify(const QString& title, const QString& body, Urgency urgency = Normal,
                const QString& icon = QString(), bool clickable = false);

signals:
    // Emitted when the user clicks a clickable notification we posted.
    void activated();

private slots:
    void onActionInvoked(uint id, const QString& actionKey);
    void onNotificationClosed(uint id, uint reason);

private:
    config::SettingsStore* settings_ = nullptr;
    bool enabled_ = true;
    QSet<uint> clickableIds_;  // ids of posted notifications that carry an action
};

}  // namespace sonar::ui
