#include "app/Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include <utility>

#include "config/SettingsStore.h"
#include "core/Log.h"

namespace sonar::update {

namespace {

constexpr auto kLatestReleaseUrl =
    "https://api.github.com/repos/Tatsu21/sonero/releases/latest";
constexpr auto kSettingsSection = "updates";
constexpr qint64 kCheckIntervalSecs = 24 * 60 * 60;
// A release bundle is ~60 MB; anything an order of magnitude past that is not a
// Sonero package and will not be written to the user's disk.
constexpr qint64 kMaxDownloadBytes = 400LL * 1024 * 1024;

QString& scheduledRelaunch() {
    static QString program;
    return program;
}

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

QHash<QString, QString> parseOsRelease(const QByteArray& text) {
    QHash<QString, QString> fields;
    for (const QByteArray& raw : text.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (line.startsWith(QLatin1Char('#')) || eq <= 0) {
            continue;
        }
        QString value = line.mid(eq + 1);
        if (value.size() >= 2 && (value.front() == QLatin1Char('"') ||
                                  value.front() == QLatin1Char('\'')) &&
            value.back() == value.front()) {
            value = value.mid(1, value.size() - 2);
        }
        fields.insert(line.left(eq), value);
    }
    return fields;
}

QString debTarget(const QHash<QString, QString>& osRelease) {
    // By the base release's codename, not by ID: Mint, Pop!_OS, Zorin and LMDE
    // name themselves but say which Ubuntu or Debian they are built on, and that
    // is what decides which Qt the package links.
    static const QHash<QString, QString> targets = {
        {QStringLiteral("jammy"), QStringLiteral("ubuntu2204")},
        {QStringLiteral("noble"), QStringLiteral("ubuntu2404")},
        {QStringLiteral("resolute"), QStringLiteral("ubuntu2604")},
        {QStringLiteral("trixie"), QStringLiteral("debian13")},
    };
    for (const char* key : {"UBUNTU_CODENAME", "DEBIAN_CODENAME", "VERSION_CODENAME"}) {
        const QString target = targets.value(osRelease.value(QString::fromLatin1(key)));
        if (!target.isEmpty()) {
            return target;
        }
    }
    return {};
}

QString currentDebTarget() {
    if (QSysInfo::currentCpuArchitecture() != QLatin1String("x86_64")) {
        return {};
    }
    QFile file(QStringLiteral("/etc/os-release"));
    if (!file.open(QIODevice::ReadOnly)) {
        file.setFileName(QStringLiteral("/usr/lib/os-release"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
    }
    return debTarget(parseOsRelease(file.readAll()));
}

void scheduleRelaunch(const QString& program) { scheduledRelaunch() = program; }

QString takeScheduledRelaunch() { return std::exchange(scheduledRelaunch(), QString()); }

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
        // "sha256:<hex>", computed by GitHub on upload — not something a
        // tampered mirror or a captive portal can produce for its own bytes.
        const QString digest = a.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QLatin1String("sha256:"))) {
            asset.sha256 = digest.mid(7).toLower();
        }
        if (!asset.name.isEmpty() && !asset.url.isEmpty()) {
            release.assets.push_back(asset);
        }
    }
    return release;
}

std::optional<Asset> pickAsset(const Release& release, Channel channel,
                               const QString& debTarget) {
    QString wanted;
    if (channel == Channel::AppImage) {
        // The portable bundle, by its exact published name. The -arch and -fedora
        // ones are CI's regression canaries: they link a newer glibc and would
        // fail to start on the very systems the portable build exists for.
        wanted = QStringLiteral("Sonero-%1-x86_64.AppImage").arg(release.version);
    } else if (channel == Channel::Deb && !debTarget.isEmpty()) {
        // packaging/collect-artifacts.sh folds the target into the version part.
        wanted = QStringLiteral("sonero_%1-%2_amd64.deb").arg(release.version, debTarget);
    } else {
        return std::nullopt;
    }
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
      running_(QString::fromLatin1(SONAR_VERSION)),
      executable_(QCoreApplication::applicationFilePath()) {
    if (channel_ == Channel::Deb) {
        debTarget_ = currentDebTarget();
    }
    if (settings_ != nullptr) {
        autoCheck_ = settings_->section(QString::fromLatin1(kSettingsSection))
                         .value(QStringLiteral("autoCheck"))
                         .toBool(true);
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

bool Updater::canInstallUpdate() const {
    return available_.has_value() && pickAsset(*available_, channel_, debTarget_).has_value();
}

void Updater::installUpdate() {
    if (busy_) {
        return;
    }
    if (!available_.has_value()) {
        emit updateFailed(QStringLiteral("There is nothing to install."));
        return;
    }
    const std::optional<Asset> asset = pickAsset(*available_, channel_, debTarget_);
    if (!asset.has_value()) {
        // Not reachable from the UI, which offers a command instead. Guarded
        // anyway: this is the one path that writes an executable to disk.
        emit updateFailed(channel_ == Channel::AppImage
                              ? QStringLiteral("Release %1 carries no portable AppImage.")
                                    .arg(available_->version)
                              : QStringLiteral("Release %1 has no package for this system.")
                                    .arg(available_->version));
        return;
    }
    const QUrl url(asset->url);
    if (!isTrustedHost(url)) {
        emit updateFailed(QStringLiteral("The download link does not point at GitHub."));
        return;
    }
    if (asset->size > kMaxDownloadBytes) {
        emit updateFailed(QStringLiteral("The published file is implausibly large."));
        return;
    }

    QDir dir;
    if (channel_ == Channel::AppImage) {
        // Next to the running bundle, never over it: the AppImage is mounted from
        // that file for as long as this process lives.
        const QString current = qEnvironmentVariable("APPIMAGE");
        dir = QFileInfo(current).absoluteDir();
        if (QFileInfo(dir.absoluteFilePath(asset->name)).absoluteFilePath() ==
            QFileInfo(current).absoluteFilePath()) {
            emit updateFailed(QStringLiteral("The new bundle has the same name as this one."));
            return;
        }
        if (!QFileInfo(dir.absolutePath()).isWritable()) {
            emit updateFailed(QStringLiteral("%1 is not writable — move Sonero somewhere "
                                             "you can write to, or download it yourself.")
                                  .arg(dir.absolutePath()));
            return;
        }
    } else {
        // Only a staging area: apt copies the package into its own cache.
        dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                   QStringLiteral("/updates"));
        if (!dir.mkpath(QStringLiteral("."))) {
            emit updateFailed(
                QStringLiteral("Could not create %1.").arg(dir.absolutePath()));
            return;
        }
    }
    const QString target = dir.absoluteFilePath(asset->name);

    busy_ = true;
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Sonero/%1 (+https://github.com/Tatsu21/sonero)")
                          .arg(running_));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = net_->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, &Updater::downloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, asset = *asset, target] {
        handleDownloadReply(reply, asset, target);
        reply->deleteLater();
    });
}

void Updater::handleDownloadReply(QNetworkReply* reply, const Asset& asset,
                                  const QString& targetPath) {
    busy_ = false;

    if (reply->error() != QNetworkReply::NoError) {
        emit updateFailed(humanNetworkError(reply));
        return;
    }
    if (!isTrustedHost(reply->url())) {
        emit updateFailed(QStringLiteral("The download was redirected off GitHub."));
        return;
    }

    const QByteArray payload = reply->readAll();
    if (payload.isEmpty() || payload.size() > kMaxDownloadBytes) {
        emit updateFailed(QStringLiteral("The download did not arrive intact."));
        return;
    }
    // Releases published before GitHub started recording digests have none; the
    // format check below is then all there is.
    if (!asset.sha256.isEmpty() &&
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() !=
            asset.sha256.toLatin1()) {
        emit updateFailed(QStringLiteral("The download does not match the checksum GitHub "
                                         "published for it."));
        return;
    }
    // Cheap, and it catches the case that actually happens when there is no
    // digest: an error page, a captive portal's login form, or a truncated
    // transfer saved under the name of something the user is about to run.
    if (channel_ == Channel::AppImage) {
        // An AppImage is an ELF binary with the AppImage type magic at offset 8.
        const bool looksLikeAppImage = payload.size() > 12 &&
                                       payload.startsWith("\x7f" "ELF") &&
                                       static_cast<unsigned char>(payload[8]) == 'A' &&
                                       static_cast<unsigned char>(payload[9]) == 'I';
        if (!looksLikeAppImage) {
            emit updateFailed(QStringLiteral("What arrived is not an AppImage."));
            return;
        }
    } else if (!payload.startsWith("!<arch>\ndebian-binary")) {
        emit updateFailed(QStringLiteral("What arrived is not a Debian package."));
        return;
    }

    const QString partPath = targetPath + QStringLiteral(".part");
    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit updateFailed(QStringLiteral("Could not write to %1.").arg(partPath));
        return;
    }
    if (part.write(payload) != payload.size() || !part.flush()) {
        part.close();
        QFile::remove(partPath);
        emit updateFailed(QStringLiteral("The download could not be saved — is the disk full?"));
        return;
    }
    part.close();

    // Executable before it is named: nothing should ever see the final name
    // while it is still a file that cannot be started. A .deb only needs to be
    // readable — apt runs as root, but reads it from here.
    QFileDevice::Permissions perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ReadGroup | QFileDevice::ReadOther;
    if (channel_ == Channel::AppImage) {
        perms |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    }
    if (!QFile::setPermissions(partPath, perms)) {
        QFile::remove(partPath);
        emit updateFailed(QStringLiteral("The download could not be made executable."));
        return;
    }
    QFile::remove(targetPath);  // a previous download of the same release
    if (!QFile::rename(partPath, targetPath)) {
        QFile::remove(partPath);
        emit updateFailed(QStringLiteral("Could not put the download in place."));
        return;
    }

    log::info("Downloaded update to {}", targetPath.toStdString());
    emit downloadFinished(targetPath);
    applyDownloaded(targetPath);
}

void Updater::applyDownloaded(const QString& path) {
    if (channel_ == Channel::AppImage) {
        // On its first start the new bundle re-points the menu entry at itself
        // and removes the one it replaced — the same path a manually downloaded
        // update takes.
        restartInto(path);
        return;
    }
    installDeb(path);
}

void Updater::installDeb(const QString& path) {
    busy_ = true;
    // pkexec, not sudo: there is no terminal to type into. polkit shows its own
    // dialog naming the command, and apt-get — not this process — writes the
    // files, runs the maintainer scripts and records the new version.
    auto* apt = new QProcess(this);
    apt->setProcessChannelMode(QProcess::MergedChannels);
    connect(apt, &QProcess::errorOccurred, this, [this, apt, path](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) {
            return;  // a crash still ends in finished(), which reports it
        }
        busy_ = false;
        apt->deleteLater();
        emit updateFailed(QStringLiteral("pkexec is not available to ask for your password. "
                                         "Install the package yourself: sudo apt install %1")
                              .arg(path));
    });
    connect(apt, &QProcess::finished, this,
            [this, apt, path](int code, QProcess::ExitStatus status) {
                busy_ = false;
                const QString output = QString::fromLocal8Bit(apt->readAll()).trimmed();
                apt->deleteLater();
                if (status == QProcess::NormalExit && code == 0) {
                    log::info("Installed {}", path.toStdString());
                    QFile::remove(path);
                    restartInto(executable_);
                    return;
                }
                log::warn("apt-get install {} failed ({}): {}", path.toStdString(), code,
                          output.toStdString());
                // 126: the dialog was dismissed. 127: not authorized, or no
                // polkit agent running to show the dialog at all.
                if (code == 126 || code == 127) {
                    emit updateFailed(QStringLiteral("Not authorized — nothing was changed."));
                    return;
                }
                const QString last = output.section(QLatin1Char('\n'), -1).trimmed();
                emit updateFailed(QStringLiteral("apt could not install it%1")
                                      .arg(last.isEmpty() ? QStringLiteral(".")
                                                          : QStringLiteral(": ") + last));
            });
    apt->start(QStringLiteral("pkexec"),
               {QStringLiteral("/usr/bin/apt-get"), QStringLiteral("install"),
                QStringLiteral("-y"), path});
}

void Updater::restartInto(const QString& program) {
    // main() starts it once this process has let go of the single-instance
    // socket; see scheduleRelaunch().
    scheduleRelaunch(program);
    QCoreApplication::quit();
}

}  // namespace sonar::update
