#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::update {

// --- What kind of install this is -------------------------------------------
// Sonero replaces only files the user downloaded; a .deb goes through apt (via
// polkit), and every other package manager is just told what to run.
enum class Channel {
    AppImage,  // a portable bundle we may replace with a newer one
    Snap,      // installed by snapd, which refreshes it on its own
    Pacman,    // installed by pacman — the AUR package, or a local makepkg
    Deb,       // installed by dpkg/apt; updated by handing apt the new .deb
    Package,   // some other package manager owns it: /usr, but neither db exists
    Source     // a build tree or a manual copy — nobody owns it but the user
};

// The facts Channel is derived from, gathered separately so the rule itself can
// be tested without an AppImage, a package database, or a particular machine.
struct InstallFacts {
    QString appImagePath;  // $APPIMAGE, empty when not running from a bundle
    QString snapName;      // $SNAP_NAME, empty when not running from a snap
    QString executablePath;
    bool hasPacmanDb = false;  // /var/lib/pacman/local
    bool hasDpkgDb = false;    // /var/lib/dpkg/status
};

[[nodiscard]] InstallFacts currentInstall();
[[nodiscard]] Channel classify(const InstallFacts& facts);
[[nodiscard]] Channel currentChannel();

// Where the user is told to go for an update we will not perform ourselves —
// the exact command, ready to paste, or an empty string when there is none.
[[nodiscard]] QString upgradeCommand(Channel channel);
[[nodiscard]] QString channelName(Channel channel);

// The CI suffix of the .deb that fits this system ("ubuntu2404", "debian13"), from
// /etc/os-release; empty when none is built for it.
[[nodiscard]] QHash<QString, QString> parseOsRelease(const QByteArray& text);
[[nodiscard]] QString debTarget(const QHash<QString, QString>& osRelease);
[[nodiscard]] QString currentDebTarget();  // also empty off x86_64: amd64 only

// --- Versions ----------------------------------------------------------------
// Only x.y.z with an optional "v"; anything else (the `dev` tag, a branch build)
// parses as nothing, so a nightly is never offered as an upgrade.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    friend bool operator==(const Version&, const Version&) = default;
    [[nodiscard]] bool operator<(const Version& other) const;
    [[nodiscard]] QString toString() const;
};

[[nodiscard]] std::optional<Version> parseVersion(const QString& text);

// True when `candidate` is strictly newer than `running`; false if either does not
// parse.
[[nodiscard]] bool isNewer(const QString& candidate, const QString& running);

// --- The published release ---------------------------------------------------
struct Asset {
    QString name;
    QString url;
    qint64 size = 0;
    QString sha256;  // lowercase hex from GitHub's "digest", empty when absent
};

struct Release {
    QString version;  // normalized, without the leading "v"
    QString pageUrl;  // the release on github.com, for "what changed"
    QString notes;
    QVector<Asset> assets;
};

// Parses GitHub's /releases/latest payload; nothing without an x.y.z tag.
[[nodiscard]] std::optional<Release> parseRelease(const QByteArray& json);

// The asset to download: the portable AppImage (never the -arch/-fedora CI
// canaries), or the .deb for `debTarget`. Nothing for other channels or when the
// release lacks it.
[[nodiscard]] std::optional<Asset> pickAsset(const Release& release, Channel channel,
                                             const QString& debTarget = QString());

// A program to start once this process is gone — taken by main() after the
// single-instance socket closes, or the new process would just wake this one.
void scheduleRelaunch(const QString& program);
[[nodiscard]] QString takeScheduledRelaunch();

// --- The updater -------------------------------------------------------------
// Checks GitHub (daily by default, can be switched off) and installs only when the
// user presses Update: an AppImage is downloaded next to the running one and
// started; a .deb is handed to apt through pkexec. Every download is checked
// against GitHub's SHA-256.
class Updater : public QObject {
    Q_OBJECT

public:
    explicit Updater(config::SettingsStore* settings, QObject* parent = nullptr);

    [[nodiscard]] Channel channel() const { return channel_; }
    [[nodiscard]] QString runningVersion() const { return running_; }

    // Persisted in the "updates" section. On unless the user turned it off.
    [[nodiscard]] bool autoCheckEnabled() const { return autoCheck_; }
    void setAutoCheckEnabled(bool on);

    // The last successful check, or an invalid time when there has not been one.
    [[nodiscard]] QString lastCheckedText() const;

    // `userInitiated` reports failures; the automatic check stays quiet about a
    // missing network.
    void checkForUpdates(bool userInitiated);

    // The automatic check, subject to the setting and the throttle. Safe to call
    // on every start; does nothing when the user switched it off.
    void checkOnStartupIfDue();

    // Download, install and restart into the pending release. On failure nothing
    // changes and updateFailed() says why.
    void installUpdate();

    [[nodiscard]] bool updateAvailable() const { return available_.has_value(); }
    // True when installUpdate() can do the job for the pending release — this
    // channel updates itself, and the release carries a file for this system.
    [[nodiscard]] bool canInstallUpdate() const;
    [[nodiscard]] Release availableRelease() const;

signals:
    void checkStarted();
    // A check finished and there is nothing newer.
    void upToDate();
    // A newer release exists. `release` is what GitHub published.
    void updateFound(const sonar::update::Release& release);
    // The check could not complete. `reason` is written for a person.
    void checkFailed(const QString& reason);

    void downloadProgress(qint64 received, qint64 total);
    // The download is on disk and verified; installing it starts now. For a .deb
    // that means the system is about to ask for the user's password.
    void downloadFinished(const QString& path);
    // installUpdate() stopped. `reason` is written for a person.
    void updateFailed(const QString& reason);

private:
    void handleCheckReply(QNetworkReply* reply, bool userInitiated);
    void handleDownloadReply(QNetworkReply* reply, const Asset& asset,
                             const QString& targetPath);
    void applyDownloaded(const QString& path);
    void installDeb(const QString& path);
    void restartInto(const QString& program);
    void rememberCheckTime();
    // This object's section of the settings file.
    [[nodiscard]] QJsonValue setting(const QString& key) const;
    void putSetting(const QString& key, const QJsonValue& value);
    [[nodiscard]] QDateTime lastCheck() const;  // invalid when never checked

    config::SettingsStore* settings_ = nullptr;
    QNetworkAccessManager* net_ = nullptr;
    Channel channel_ = Channel::Source;
    QString running_;
    // Taken at startup: once apt has replaced the binary, /proc/self/exe reads
    // "/usr/bin/Sonero (deleted)", and that is not something to relaunch.
    QString executable_;
    QString debTarget_;
    bool autoCheck_ = true;
    bool busy_ = false;  // one request at a time; the UI disables its buttons too
    std::optional<Release> available_;
};

}  // namespace sonar::update
