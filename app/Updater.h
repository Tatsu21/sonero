#pragma once

#include <QByteArray>
#include <QHash>
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
//
// It decides what "update" is even allowed to mean. Replacing a file the user
// downloaded is Sonero's business. Files a package manager owns are only ever
// replaced through that package manager: a .deb is handed to apt (with the
// user's password, via polkit), and everything else is told what to run.
enum class Channel {
    AppImage,  // a portable bundle we may replace with a newer one
    Pacman,    // installed by pacman — the AUR package, or a local makepkg
    Deb,       // installed by dpkg/apt; updated by handing apt the new .deb
    Package,   // some other package manager owns it: /usr, but neither db exists
    Source     // a build tree or a manual copy — nobody owns it but the user
};

// The facts Channel is derived from, gathered separately so the rule itself can
// be tested without an AppImage, a package database, or a particular machine.
struct InstallFacts {
    QString appImagePath;  // $APPIMAGE, empty when not running from a bundle
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

// Which of the release's .deb builds fits this system, as the suffix CI gives
// it ("ubuntu2404", "debian13"), from /etc/os-release. Empty for a system no
// .deb is built for — a .deb links its distribution's own Qt, so the wrong one
// does not install, and guessing is worse than saying so.
[[nodiscard]] QHash<QString, QString> parseOsRelease(const QByteArray& text);
[[nodiscard]] QString debTarget(const QHash<QString, QString>& osRelease);
[[nodiscard]] QString currentDebTarget();  // also empty off x86_64: amd64 only

// --- Versions ----------------------------------------------------------------
//
// Only x.y.z, with an optional leading "v". Everything else — the moving `dev`
// tag, a branch build — parses as nothing, which is what keeps a nightly from
// ever being offered as an upgrade over a release.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    friend bool operator==(const Version&, const Version&) = default;
    [[nodiscard]] bool operator<(const Version& other) const;
    [[nodiscard]] QString toString() const;
};

[[nodiscard]] std::optional<Version> parseVersion(const QString& text);

// True when `candidate` is a release strictly newer than `running`. False for
// anything unparseable on either side: a build that cannot say what it is does
// not get told it is out of date.
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

// Parses GitHub's /releases/latest payload. Returns nothing when the JSON is not
// a release object or carries no x.y.z tag — a prerelease is never returned by
// that endpoint, so `dev` builds are out of scope by construction.
[[nodiscard]] std::optional<Release> parseRelease(const QByteArray& json);

// The file to download for this install. For an AppImage, the portable bundle,
// built on the oldest glibc — the -arch and -fedora bundles exist to catch CI
// regressions and run on fewer systems, so they are never chosen automatically.
// For a .deb, the build for `debTarget` (see debTarget()). Returns nothing for
// every other channel, and when the release carries no such asset.
[[nodiscard]] std::optional<Asset> pickAsset(const Release& release, Channel channel,
                                             const QString& debTarget = QString());

// Set by an update that wants a new process started once this one is gone, and
// taken by main() after the single-instance socket is closed: started any
// earlier, the new process finds this one still answering, asks it to come
// forward, and exits — and then this one quits too.
void scheduleRelaunch(const QString& program);
[[nodiscard]] QString takeScheduledRelaunch();

// --- The updater -------------------------------------------------------------
//
// Checks GitHub for a newer release and, when the user presses Update, installs
// it and restarts into it:
//
//   AppImage  the new bundle is downloaded next to the running one (never over
//             it) and started; on its first run Sonero's existing bookkeeping
//             re-points the menu entry and removes the old file.
//   Deb       the .deb built for this distribution is downloaded to the cache
//             and handed to apt through pkexec, so the user authorizes it with
//             their password and dpkg stays the owner of every file.
//
// Every download is checked against the SHA-256 GitHub publishes for it. The
// once-a-day check is on by default and can be switched off; nothing is ever
// downloaded or installed without the button being pressed.
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

    // Start a check. `userInitiated` reports failures and ignores the once-a-day
    // throttle; the automatic path stays quiet about a network that is simply
    // not there, which on a laptop is most of the time.
    void checkForUpdates(bool userInitiated);

    // The automatic check, subject to the setting and the throttle. Safe to call
    // on every start; does nothing when the user switched it off.
    void checkOnStartupIfDue();

    // Download the pending release, install it and restart into it. On success
    // this process quits; on any failure it stays exactly as it was and
    // updateFailed() says why.
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
