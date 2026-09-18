#include <QtTest>

#include <QByteArray>
#include <QString>

#include "app/Updater.h"

namespace update = sonar::update;

// The decisions that decide whether a user is offered an update, and which file
// they are offered — all of them pure, none of them needing a network, an
// AppImage or a particular distribution to exercise.
class UpdaterTest : public QObject {
    Q_OBJECT

private slots:
    void parsesReleaseVersions();
    void rejectsAnythingThatIsNotARelease();
    void comparesVersionsByComponent();
    void neverOffersAnUpdateToAnUnnamedBuild();
    void readsAGitHubRelease();
    void rejectsAPayloadWithoutAReleaseTag();
    void picksThePortableBundleOnly();
    void offersNoAssetToPackagedInstalls();
    void classifiesAppImageBeforeAnythingElse();
    void classifiesSystemPrefixesByPackageDatabase();
    void classifiesEverythingElseAsALocalBuild();
    void namesTheCommandForChannelsItWillNotUpdate();

private:
    // A trimmed-down copy of what GitHub's /releases/latest returns.
    static QByteArray releasePayload();
};

QByteArray UpdaterTest::releasePayload() {
    return QByteArrayLiteral(R"({
        "tag_name": "0.2.0",
        "html_url": "https://github.com/Tatsu21/sonero/releases/tag/0.2.0",
        "body": "Nothing to see here.",
        "assets": [
            {"name": "sonero_0.2.0-ubuntu2204_amd64.deb",
             "browser_download_url": "https://github.com/x/sonero_0.2.0.deb", "size": 4},
            {"name": "Sonero-0.2.0-arch-x86_64.AppImage",
             "browser_download_url": "https://github.com/x/arch.AppImage", "size": 2},
            {"name": "Sonero-0.2.0-x86_64.AppImage",
             "browser_download_url": "https://github.com/x/portable.AppImage", "size": 3},
            {"name": "Sonero-0.2.0-fedora-x86_64.AppImage",
             "browser_download_url": "https://github.com/x/fedora.AppImage", "size": 1}
        ]
    })");
}

void UpdaterTest::parsesReleaseVersions() {
    const auto plain = update::parseVersion(QStringLiteral("0.1.2"));
    QVERIFY(plain.has_value());
    QCOMPARE(plain->major, 0);
    QCOMPARE(plain->minor, 1);
    QCOMPARE(plain->patch, 2);

    // Both tag spellings the build accepts.
    const auto prefixed = update::parseVersion(QStringLiteral("v10.20.30"));
    QVERIFY(prefixed.has_value());
    QCOMPARE(prefixed->toString(), QStringLiteral("10.20.30"));

    // Whitespace is what a hand-edited settings file or a stray newline brings.
    QVERIFY(update::parseVersion(QStringLiteral("  1.0.0\n")).has_value());
}

void UpdaterTest::rejectsAnythingThatIsNotARelease() {
    // The repository carries a moving `dev` tag, and nightly builds name
    // themselves after a branch and a commit. Neither is a release, and neither
    // may be compared against one.
    QVERIFY(!update::parseVersion(QStringLiteral("dev")).has_value());
    QVERIFY(!update::parseVersion(QStringLiteral("0.1.2 (main@1a2b3c)")).has_value());
    QVERIFY(!update::parseVersion(QStringLiteral("0.1")).has_value());
    QVERIFY(!update::parseVersion(QStringLiteral("0.1.2-rc1")).has_value());
    QVERIFY(!update::parseVersion(QString()).has_value());
}

void UpdaterTest::comparesVersionsByComponent() {
    QVERIFY(update::isNewer(QStringLiteral("0.1.3"), QStringLiteral("0.1.2")));
    QVERIFY(update::isNewer(QStringLiteral("0.2.0"), QStringLiteral("0.1.9")));
    QVERIFY(update::isNewer(QStringLiteral("1.0.0"), QStringLiteral("0.99.99")));

    // Same version: not an update. Older: certainly not — a release that was
    // pulled and replaced by an earlier one must not drag the user backwards.
    QVERIFY(!update::isNewer(QStringLiteral("0.1.2"), QStringLiteral("0.1.2")));
    QVERIFY(!update::isNewer(QStringLiteral("0.1.1"), QStringLiteral("0.1.2")));

    // Numeric, not lexicographic: "0.1.10" sorts before "0.1.9" as text.
    QVERIFY(update::isNewer(QStringLiteral("0.1.10"), QStringLiteral("0.1.9")));
}

void UpdaterTest::neverOffersAnUpdateToAnUnnamedBuild() {
    // A build from a tarball with no tag calls itself 0.0.0, and a nightly calls
    // itself something unparseable. Telling either that it is out of date would
    // be a guess, and acting on it would replace a build the user chose.
    QVERIFY(!update::isNewer(QStringLiteral("0.1.3"), QStringLiteral("dev")));
    QVERIFY(!update::isNewer(QStringLiteral("dev"), QStringLiteral("0.1.2")));
    QVERIFY(update::isNewer(QStringLiteral("0.1.3"), QStringLiteral("0.0.0")));
}

void UpdaterTest::readsAGitHubRelease() {
    const auto release = update::parseRelease(releasePayload());
    QVERIFY(release.has_value());
    QCOMPARE(release->version, QStringLiteral("0.2.0"));
    QCOMPARE(release->pageUrl,
             QStringLiteral("https://github.com/Tatsu21/sonero/releases/tag/0.2.0"));
    QCOMPARE(static_cast<int>(release->assets.size()), 4);
}

void UpdaterTest::rejectsAPayloadWithoutAReleaseTag() {
    QVERIFY(!update::parseRelease(QByteArrayLiteral("not json at all")).has_value());
    QVERIFY(!update::parseRelease(QByteArrayLiteral("[]")).has_value());
    // GitHub answers a repository with no releases with a message object, and
    // the `dev` prerelease is published under a tag that is not a version.
    QVERIFY(!update::parseRelease(QByteArrayLiteral(R"({"message":"Not Found"})")).has_value());
    QVERIFY(!update::parseRelease(QByteArrayLiteral(R"({"tag_name":"dev"})")).has_value());
}

void UpdaterTest::picksThePortableBundleOnly() {
    const auto release = update::parseRelease(releasePayload());
    QVERIFY(release.has_value());

    const auto asset = update::pickAsset(*release, update::Channel::AppImage);
    QVERIFY(asset.has_value());
    // Not the -arch or -fedora bundle: they link a newer glibc and would fail to
    // start on the systems the portable build exists for.
    QCOMPARE(asset->name, QStringLiteral("Sonero-0.2.0-x86_64.AppImage"));
    QCOMPARE(asset->url, QStringLiteral("https://github.com/x/portable.AppImage"));

    // A release that published no portable bundle offers nothing at all, rather
    // than falling back to whatever else is lying around.
    update::Release without = *release;
    without.assets.removeIf(
        [](const update::Asset& a) { return a.name.contains(QLatin1String("-0.2.0-x86_64")); });
    QVERIFY(!update::pickAsset(without, update::Channel::AppImage).has_value());
}

void UpdaterTest::offersNoAssetToPackagedInstalls() {
    const auto release = update::parseRelease(releasePayload());
    QVERIFY(release.has_value());
    // The .deb is in the payload, and is still not offered: dpkg owns those
    // files, and an app that writes to them is a bug report waiting to happen.
    QVERIFY(!update::pickAsset(*release, update::Channel::Deb).has_value());
    QVERIFY(!update::pickAsset(*release, update::Channel::Pacman).has_value());
    QVERIFY(!update::pickAsset(*release, update::Channel::Source).has_value());
}

void UpdaterTest::classifiesAppImageBeforeAnythingElse() {
    update::InstallFacts facts;
    facts.appImagePath = QStringLiteral("/home/u/Downloads/Sonero-0.1.2-x86_64.AppImage");
    // An AppImage mounts itself under /tmp and runs on a machine that has a
    // package database of its own; $APPIMAGE is the only fact that matters.
    facts.executablePath = QStringLiteral("/tmp/.mount_Sonerox/usr/bin/Sonero");
    facts.hasPacmanDb = true;
    QCOMPARE(update::classify(facts), update::Channel::AppImage);
}

void UpdaterTest::classifiesSystemPrefixesByPackageDatabase() {
    update::InstallFacts facts;
    facts.executablePath = QStringLiteral("/usr/bin/Sonero");

    facts.hasPacmanDb = true;
    QCOMPARE(update::classify(facts), update::Channel::Pacman);

    // An Arch machine with dpkg installed is still an Arch machine.
    facts.hasDpkgDb = true;
    QCOMPARE(update::classify(facts), update::Channel::Pacman);

    facts.hasPacmanDb = false;
    QCOMPARE(update::classify(facts), update::Channel::Deb);

    facts.hasDpkgDb = false;
    QCOMPARE(update::classify(facts), update::Channel::Package);
}

void UpdaterTest::classifiesEverythingElseAsALocalBuild() {
    update::InstallFacts facts;
    facts.executablePath = QStringLiteral("/home/u/sonero/build/Sonero");
    facts.hasPacmanDb = true;
    QCOMPARE(update::classify(facts), update::Channel::Source);
}

void UpdaterTest::namesTheCommandForChannelsItWillNotUpdate() {
    // -Syu, not -S: upgrading one package against an un-synced rolling system is
    // how a machine ends up half upgraded.
    QVERIFY(update::upgradeCommand(update::Channel::Pacman).contains(QLatin1String("-Syu")));
    QVERIFY(update::upgradeCommand(update::Channel::Deb).contains(QLatin1String("apt")));
    // The AppImage has a button instead, so there is nothing to paste.
    QVERIFY(update::upgradeCommand(update::Channel::AppImage).isEmpty());
}

QTEST_GUILESS_MAIN(UpdaterTest)
#include "UpdaterTest.moc"
