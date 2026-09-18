#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace sonar::config {
class SettingsStore;
}

namespace sonar::update {

// --- What kind of install this is -------------------------------------------
//
// It decides what "update" is even allowed to mean. Replacing a file the user
// downloaded is Sonero's business; replacing files a package manager owns is
// not, so for packaged installs the app only ever says what to run.
enum class Channel {
    AppImage,  // a portable bundle we may replace with a newer one
    Pacman,    // installed by pacman — the AUR package, or a local makepkg
    Deb,       // installed by dpkg/apt
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

// The bundle to download for an AppImage install: the portable one, built on the
// oldest glibc. The -arch and -fedora bundles exist to catch CI regressions and
// run on fewer systems, so they are never chosen automatically. Returns nothing
// for every other channel, and when the release carries no such asset.
[[nodiscard]] std::optional<Asset> pickAsset(const Release& release, Channel channel);

// --- The updater -------------------------------------------------------------
//
// Checks GitHub for a newer release, and — on an AppImage install only —
// downloads the new bundle next to the running one. It never replaces the
// running file and never installs anything on its own: the download lands beside
// the current bundle and the user restarts into it, at which point Sonero's
// existing bookkeeping re-points the menu entry and removes the old file.
//
// Nothing here runs unless the user asked for it, either by switching the
// automatic check on or by pressing Check now. Off by default: contacting a
// server is not something an audio mixer should do uninvited.
class Updater : public QObject {
    Q_OBJECT

public:
    explicit Updater(config::SettingsStore* settings, QObject* parent = nullptr);

    [[nodiscard]] Channel channel() const { return channel_; }
    [[nodiscard]] QString runningVersion() const { return running_; }

    // Persisted in the "updates" section. Off unless the user turned it on.
    [[nodiscard]] bool autoCheckEnabled() const { return autoCheck_; }
    void setAutoCheckEnabled(bool on);

    // The last successful check, or an invalid time when there has not been one.
    [[nodiscard]] QString lastCheckedText() const;

    // Start a check. `userInitiated` reports failures and ignores the once-a-day
    // throttle; the automatic path stays quiet about a network that is simply
    // not there, which on a laptop is most of the time.
    void checkForUpdates(bool userInitiated);

    // The automatic check, subject to the setting and the throttle. Safe to call
    // on every start; does nothing when the user has not opted in.
    void checkOnStartupIfDue();

    // Download the release's AppImage next to the running bundle. Only valid on
    // an AppImage install with a pending update; emits downloadFailed otherwise.
    void downloadUpdate();

    // Launch the downloaded bundle and quit this one. Returns false when nothing
    // has been downloaded, or the new bundle could not be started — in which
    // case this process stays exactly as it was.
    bool restartIntoDownloadedUpdate();

    [[nodiscard]] bool updateAvailable() const { return available_.has_value(); }
    [[nodiscard]] Release availableRelease() const;
    [[nodiscard]] QString downloadedBundlePath() const { return downloaded_; }

signals:
    void checkStarted();
    // A check finished and there is nothing newer.
    void upToDate();
    // A newer release exists. `release` is what GitHub published.
    void updateFound(const sonar::update::Release& release);
    // The check could not complete. `reason` is written for a person.
    void checkFailed(const QString& reason);

    void downloadProgress(qint64 received, qint64 total);
    // The bundle is on disk at `path`, executable, ready to be started.
    void downloadFinished(const QString& path);
    void downloadFailed(const QString& reason);

private:
    void handleCheckReply(QNetworkReply* reply, bool userInitiated);
    void handleDownloadReply(QNetworkReply* reply, const QString& targetPath,
                             const QString& partPath);
    void rememberCheckTime();

    config::SettingsStore* settings_ = nullptr;
    QNetworkAccessManager* net_ = nullptr;
    Channel channel_ = Channel::Source;
    QString running_;
    bool autoCheck_ = false;
    bool busy_ = false;  // one request at a time; the UI disables its buttons too
    std::optional<Release> available_;
    QString downloaded_;  // path of a finished download, empty until then
};

}  // namespace sonar::update
