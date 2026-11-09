#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

namespace sonar::config {
class SettingsStore;
}

namespace sonar::ui {

// Desktop notifications over org.freedesktop.Notifications; the on/off preference
// is persisted. A clickable notification's action comes back as activated().
class Notifier : public QObject {
    Q_OBJECT

public:
    enum Urgency { Low = 0, Normal = 1, Critical = 2 };

    explicit Notifier(config::SettingsStore* settings, QObject* parent = nullptr);

    [[nodiscard]] bool enabled() const { return enabled_; }
    void setEnabled(bool on);  // persists to the settings store

    // Fire-and-forget (no-op when disabled). `icon` is a themed name; `clickable` adds an
    // action that emits activated(); a non-empty `key` replaces the previous status.
    void notify(const QString& title, const QString& body, Urgency urgency = Normal,
                const QString& icon = QString(), bool clickable = false,
                const QString& key = QString());

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
    QHash<QString, uint> keyedIds_;  // key -> id of the notification it last posted
};

}  // namespace sonar::ui
