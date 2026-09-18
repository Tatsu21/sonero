#include "app/Updater.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

#include "config/SettingsStore.h"
#include "core/Log.h"

namespace sonar::update {

namespace {

constexpr auto kLatestReleaseUrl =
    "https://api.github.com/repos/Tatsu21/sonero/releases/latest";
constexpr auto kSettingsSection = "updates";
constexpr qint64 kCheckIntervalSecs = 24 * 60 * 60;
// A release bundle is ~60 MB; anything an order of magnitude past that is not a
// Sonero AppImage and will not be written to the user's disk.
constexpr qint64 kMaxDownloadBytes = 400LL * 1024 * 1024;

// Redirects are followed, so what matters is where the bytes actually came from.
// GitHub hands release assets off to its object storage, and both of those hosts
// are acceptable; nothing else is.
bool isTrustedHost(const QUrl& url) {
    if (url.scheme() != QLatin1String("https")) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com") || host == QLatin1String("api.github.com") ||
           host.endsWith(QLatin1String(".githubusercontent.com"));
}

QString humanNetworkError(QNetworkReply* reply) {
    switch (reply->error()) {
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::UnknownNetworkError:
        case QNetworkReply::TemporaryNetworkFailureError:
            return QStringLiteral("No connection to github.com.");
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            return QStringLiteral("github.com did not answer in time.");
        case QNetworkReply::ContentNotFoundError:
            return QStringLiteral("There is no published release to compare against yet.");
        default:
            break;
    }
    // 403 from the API is almost always the unauthenticated rate limit, which is
    // per address and shared with anything else on the network.
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429) {
        return QStringLiteral("GitHub is rate-limiting this address. Try again later.");
    }
    if (status > 0) {
        return QStringLiteral("github.com answered %1.").arg(status);
    }
    return reply->errorString();
}

}  // namespace

// --- Install channel ---------------------------------------------------------

InstallFacts currentInstall() {
    InstallFacts facts;
    facts.appImagePath = qEnvironmentVariable("APPIMAGE");
    facts.executablePath = QCoreApplication::applicationFilePath();
    facts.hasPacmanDb = QFileInfo::exists(QStringLiteral("/var/lib/pacman/local"));
    facts.hasDpkgDb = QFileInfo::exists(QStringLiteral("/var/lib/dpkg/status"));
    return facts;
}

Channel classify(const InstallFacts& facts) {
    // $APPIMAGE is set by the runtime itself and points at the bundle, so it
    // settles the question before any path guessing starts.
    if (!facts.appImagePath.isEmpty()) {
        return Channel::AppImage;
    }
    // Everything under a system prefix got there through someone's package
    // manager — or through `make install`, which for our purposes is the same
    // thing: not ours to overwrite.
    const bool systemPrefix = facts.executablePath.startsWith(QLatin1String("/usr/")) ||
                              facts.executablePath.startsWith(QLatin1String("/opt/"));
    if (!systemPrefix) {
        return Channel::Source;
    }
    // Checked in this order because a pacman database on a machine that also has
    // dpkg is an Arch system with a dpkg tool installed, not the other way round.
    if (facts.hasPacmanDb) {
        return Channel::Pacman;
    }
    if (facts.hasDpkgDb) {
        return Channel::Deb;
    }
    return Channel::Package;
}

Channel currentChannel() { return classify(currentInstall()); }

QString upgradeCommand(Channel channel) {
    switch (channel) {
        case Channel::Pacman:
            // -Syu rather than -S: on a rolling release, upgrading one package
            // against an un-synced system is how you get a half-upgraded machine.
            return QStringLiteral("yay -Syu sonero");
        case Channel::Deb:
            return QStringLiteral("sudo apt install ./sonero_*_amd64.deb");
        case Channel::Source:
            return QStringLiteral("git pull && cmake --build build --parallel");
        case Channel::AppImage:
        case Channel::Package:
            break;
    }
    return {};
}

QString channelName(Channel channel) {
    switch (channel) {
        case Channel::AppImage:
            return QStringLiteral("AppImage");
        case Channel::Pacman:
            return QStringLiteral("pacman package");
        case Channel::Deb:
            return QStringLiteral("Debian package");
        case Channel::Package:
            return QStringLiteral("system package");
        case Channel::Source:
            return QStringLiteral("local build");
    }
    return QStringLiteral("local build");
}

// --- Versions ----------------------------------------------------------------

bool Version::operator<(const Version& other) const {
    if (major != other.major) {
        return major < other.major;
    }
    if (minor != other.minor) {
        return minor < other.minor;
    }
    return patch < other.patch;
}

QString Version::toString() const {
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

std::optional<Version> parseVersion(const QString& text) {
    // Anchored on purpose: "0.1.2 (main@1a2b3c)" is a build id, not a release,
    // and a build that cannot name a release must not be compared to one.
    static const QRegularExpression re(QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)$"));
    const QRegularExpressionMatch m = re.match(text.trimmed());
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    Version v;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).toInt();
    v.patch = m.captured(3).toInt();
    return v;
}

bool isNewer(const QString& candidate, const QString& running) {
    const std::optional<Version> a = parseVersion(candidate);
    const std::optional<Version> b = parseVersion(running);
    if (!a.has_value() || !b.has_value()) {
        return false;
    }
    return *b < *a;
}

// --- The published release ---------------------------------------------------

std::optional<Release> parseRelease(const QByteArray& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();
    const std::optional<Version> version =
        parseVersion(obj.value(QStringLiteral("tag_name")).toString());
    if (!version.has_value()) {
        return std::nullopt;
    }

    Release release;
    release.version = version->toString();
    release.pageUrl = obj.value(QStringLiteral("html_url")).toString();
    release.notes = obj.value(QStringLiteral("body")).toString();

    for (const QJsonValue& value : obj.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = value.toObject();
        Asset asset;
        asset.name = a.value(QStringLiteral("name")).toString();
        asset.url = a.value(QStringLiteral("browser_download_url")).toString();
        asset.size = static_cast<qint64>(a.value(QStringLiteral("size")).toDouble());
        if (!asset.name.isEmpty() && !asset.url.isEmpty()) {
            release.assets.push_back(asset);
        }
    }
    return release;
}

std::optional<Asset> pickAsset(const Release& release, Channel channel) {
    if (channel != Channel::AppImage) {
        return std::nullopt;
    }
    // The portable bundle, by its exact published name. The -arch and -fedora
    // ones are CI's regression canaries: they link a newer glibc and would fail
    // to start on the very systems the portable build exists for.
    const QString wanted =
        QStringLiteral("Sonero-%1-x86_64.AppImage").arg(release.version);
    for (const Asset& asset : release.assets) {
        if (asset.name == wanted) {
            return asset;
        }
    }
    return std::nullopt;
}

// --- Updater -----------------------------------------------------------------

Updater::Updater(config::SettingsStore* settings, QObject* parent)
    : QObject(parent),
      settings_(settings),
      net_(new QNetworkAccessManager(this)),
      channel_(currentChannel()),
      running_(QString::fromLatin1(SONAR_VERSION)) {
    if (settings_ != nullptr) {
        autoCheck_ = settings_->section(QString::fromLatin1(kSettingsSection))
                         .value(QStringLiteral("autoCheck"))
                         .toBool(false);
    }
}

void Updater::setAutoCheckEnabled(bool on) {
    autoCheck_ = on;
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject section = settings_->section(QString::fromLatin1(kSettingsSection));
    section[QStringLiteral("autoCheck")] = on;
    settings_->putSection(QString::fromLatin1(kSettingsSection), section);
}

QString Updater::lastCheckedText() const {
    if (settings_ == nullptr) {
        return {};
    }
    const QString stamp = settings_->section(QString::fromLatin1(kSettingsSection))
                              .value(QStringLiteral("lastCheck"))
                              .toString();
    const QDateTime when = QDateTime::fromString(stamp, Qt::ISODate);
    if (!when.isValid()) {
        return {};
    }
    return when.toLocalTime().toString(QStringLiteral("d MMM yyyy, HH:mm"));
}

void Updater::rememberCheckTime() {
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject section = settings_->section(QString::fromLatin1(kSettingsSection));
    section[QStringLiteral("lastCheck")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    settings_->putSection(QString::fromLatin1(kSettingsSection), section);
}

void Updater::checkOnStartupIfDue() {
    if (!autoCheck_ || settings_ == nullptr) {
        return;
    }
    const QString stamp = settings_->section(QString::fromLatin1(kSettingsSection))
                              .value(QStringLiteral("lastCheck"))
                              .toString();
    const QDateTime last = QDateTime::fromString(stamp, Qt::ISODate);
    // An invalid stamp (first run, or a hand-edited file) counts as due. A stamp
    // in the future — a clock that was wrong and got fixed — does too, rather
    // than locking the check out until the date catches up.
    if (last.isValid()) {
        const qint64 age = last.secsTo(QDateTime::currentDateTimeUtc());
        if (age >= 0 && age < kCheckIntervalSecs) {
            return;
        }
    }
    checkForUpdates(/*userInitiated=*/false);
}

void Updater::checkForUpdates(bool userInitiated) {
    if (busy_) {
        return;
    }
    busy_ = true;
    emit checkStarted();

    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseUrl))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Sonero/%1 (+https://github.com/Tatsu21/sonero)")
                          .arg(running_));
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = net_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, userInitiated] {
        handleCheckReply(reply, userInitiated);
        reply->deleteLater();
    });
}

void Updater::handleCheckReply(QNetworkReply* reply, bool userInitiated) {
    busy_ = false;

    if (reply->error() != QNetworkReply::NoError) {
        const QString reason = humanNetworkError(reply);
        log::warn("Update check failed: {}", reason.toStdString());
        // The automatic check stays silent: a laptop that woke up without Wi-Fi
        // should not greet its owner with an error they did not ask for.
        if (userInitiated) {
            emit checkFailed(reason);
        }
        return;
    }
    if (!isTrustedHost(reply->url())) {
        if (userInitiated) {
            emit checkFailed(QStringLiteral("The answer came from an unexpected server."));
        }
        return;
    }

    const std::optional<Release> release = parseRelease(reply->readAll());
    if (!release.has_value()) {
        if (userInitiated) {
            emit checkFailed(QStringLiteral("GitHub's answer could not be read."));
        }
        return;
    }

    rememberCheckTime();

    if (!isNewer(release->version, running_)) {
        available_.reset();
        log::info("Update check: {} is current (latest release {})", running_.toStdString(),
                  release->version.toStdString());
        emit upToDate();
        return;
    }

    available_ = release;
    log::info("Update available: {} (running {})", release->version.toStdString(),
              running_.toStdString());
    emit updateFound(*release);
}

Release Updater::availableRelease() const {
    return available_.value_or(Release{});
}

void Updater::downloadUpdate() {
    if (busy_) {
        return;
    }
    if (channel_ != Channel::AppImage) {
        // Not reachable from the UI, which offers a command instead. Guarded
        // anyway: this is the one path that writes an executable to disk.
        emit downloadFailed(QStringLiteral("Only an AppImage install updates itself."));
        return;
    }
    if (!available_.has_value()) {
        emit downloadFailed(QStringLiteral("There is nothing to download."));
        return;
    }
    const std::optional<Asset> asset = pickAsset(*available_, channel_);
    if (!asset.has_value()) {
        emit downloadFailed(
            QStringLiteral("Release %1 carries no portable AppImage.").arg(available_->version));
        return;
    }
    const QUrl url(asset->url);
    if (!isTrustedHost(url)) {
        emit downloadFailed(QStringLiteral("The download link does not point at GitHub."));
        return;
    }
    if (asset->size > kMaxDownloadBytes) {
        emit downloadFailed(QStringLiteral("The published bundle is implausibly large."));
        return;
    }

    // Next to the running bundle, never over it: the AppImage is mounted from
    // that file for as long as this process lives, and the user keeps whatever
    // they have until they choose to start the new one.
    const QString current = qEnvironmentVariable("APPIMAGE");
    const QDir dir = QFileInfo(current).absoluteDir();
    const QString target = dir.absoluteFilePath(asset->name);
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(current).absoluteFilePath()) {
        emit downloadFailed(QStringLiteral("The new bundle has the same name as this one."));
        return;
    }
    const QFileInfo dirInfo(dir.absolutePath());
    if (!dirInfo.isWritable()) {
        emit downloadFailed(QStringLiteral("%1 is not writable — move Sonero somewhere "
                                           "you can write to, or download it yourself.")
                                .arg(dir.absolutePath()));
        return;
    }

    const QString partPath = target + QStringLiteral(".part");
    QFile::remove(partPath);  // a previous attempt that died mid-download

    busy_ = true;
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Sonero/%1 (+https://github.com/Tatsu21/sonero)")
                          .arg(running_));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = net_->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, &Updater::downloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, target, partPath] {
        handleDownloadReply(reply, target, partPath);
        reply->deleteLater();
    });
}

void Updater::handleDownloadReply(QNetworkReply* reply, const QString& targetPath,
                                  const QString& partPath) {
    busy_ = false;

    if (reply->error() != QNetworkReply::NoError) {
        emit downloadFailed(humanNetworkError(reply));
        return;
    }
    if (!isTrustedHost(reply->url())) {
        emit downloadFailed(QStringLiteral("The download was redirected off GitHub."));
        return;
    }

    const QByteArray payload = reply->readAll();
    if (payload.isEmpty() || payload.size() > kMaxDownloadBytes) {
        emit downloadFailed(QStringLiteral("The download did not arrive intact."));
        return;
    }
    // An AppImage is an ELF binary with the AppImage type magic at offset 8.
    // Cheap, and it catches the case that actually happens: an error page, a
    // captive portal's login form, or a truncated transfer saved as an
    // executable the user is then invited to run.
    const bool looksLikeElf = payload.size() > 12 && payload.startsWith("\x7f" "ELF") &&
                              static_cast<unsigned char>(payload[8]) == 'A' &&
                              static_cast<unsigned char>(payload[9]) == 'I';
    if (!looksLikeElf) {
        emit downloadFailed(QStringLiteral("What arrived is not an AppImage."));
        return;
    }

    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(QStringLiteral("Could not write to %1.").arg(partPath));
        return;
    }
    if (part.write(payload) != payload.size() || !part.flush()) {
        part.close();
        QFile::remove(partPath);
        emit downloadFailed(QStringLiteral("The download could not be saved — is the disk full?"));
        return;
    }
    part.close();

    // Executable before it is named: nothing should ever see the final name
    // while it is still a file that cannot be started.
    if (!QFile::setPermissions(partPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                             QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                             QFileDevice::ExeOther)) {
        QFile::remove(partPath);
        emit downloadFailed(QStringLiteral("The downloaded bundle could not be made executable."));
        return;
    }
    QFile::remove(targetPath);  // a previous download of the same release
    if (!QFile::rename(partPath, targetPath)) {
        QFile::remove(partPath);
        emit downloadFailed(QStringLiteral("Could not put the new bundle in place."));
        return;
    }

    downloaded_ = targetPath;
    log::info("Downloaded update to {}", targetPath.toStdString());
    emit downloadFinished(targetPath);
}

bool Updater::restartIntoDownloadedUpdate() {
    if (downloaded_.isEmpty() || !QFileInfo::exists(downloaded_)) {
        return false;
    }
    // Detached on purpose: the new bundle outlives this process, and on its first
    // start it re-points the menu entry at itself and removes the one it
    // replaced — the same path a manually downloaded update takes.
    if (!QProcess::startDetached(downloaded_, {})) {
        log::warn("Could not start {}", downloaded_.toStdString());
        return false;
    }
    QCoreApplication::quit();
    return true;
}

}  // namespace sonar::update
