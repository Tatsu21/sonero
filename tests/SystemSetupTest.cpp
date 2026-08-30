#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include "app/SystemSetup.h"

namespace setup = sonar::setup;

class SystemSetupTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void findsPackagedResources();
    void udevScriptIsExactAndScoped();
    void bluezScriptIsReversible();
    void noScriptForNonPrivilegedItems();
    void appImageDetectionFollowsEnvironment();
    void checksAlwaysReportCoreItems();

    // Superseded-bundle cleanup.
    void removesTheBundleThatRanBefore();
    void keepsTheRunningBundle();
    void keepsABundleReachedBySymlink();
    void keepsABundleReachedByAnUncanonicalPath();
    void refusesAnythingThatIsNotABundleFile();
    void doesNothingWhenNotRunningFromABundle();
    void readsThePreviousBundleFromTheMenuEntry();

private:
    // The menu entry SystemSetup writes, pointing at `program`.
    static void writeMenuEntryPointingAt(const QString& program);
    // An empty file that looks like a downloaded bundle.
    static QString makeBundle(const QString& name);
    static QString scratchDir();
};

void SystemSetupTest::findsPackagedResources() {
    // The udev rule must be locatable, otherwise the fix silently does nothing.
    const QString rule =
        setup::resourcePath(QStringLiteral("udev/70-sonero-steelseries.rules"));
    QVERIFY2(!rule.isEmpty(), "packaged udev rule not found");
    QVERIFY(QFileInfo::exists(rule));

    QVERIFY(!setup::resourcePath(QStringLiteral("icons/sonero-256.png")).isEmpty());
    QVERIFY(setup::resourcePath(QStringLiteral("nope/does-not-exist")).isEmpty());
}

void SystemSetupTest::udevScriptIsExactAndScoped() {
    const QString s = setup::privilegedFixScript(setup::CheckId::UdevRule);
    QVERIFY(!s.isEmpty());

    // Installs the rule read-only and reloads udev — nothing else.
    QVERIFY(s.contains(QStringLiteral("install -m 0644")));
    QVERIFY(s.contains(QStringLiteral("/etc/udev/rules.d/70-sonero-steelseries.rules")));
    QVERIFY(s.contains(QStringLiteral("udevadm control --reload-rules")));
    QVERIFY(s.contains(QStringLiteral("udevadm trigger --subsystem-match=hidraw")));

    // Guard against a destructive edit sneaking into the privileged path.
    for (const char* forbidden : {"rm -rf", "mkfs", "dd ", "chmod 777", "curl", "wget", ">/dev/sd"}) {
        QVERIFY2(!s.contains(QLatin1String(forbidden)),
                 qPrintable(QStringLiteral("privileged script contains %1")
                                .arg(QLatin1String(forbidden))));
    }
    // Aborts on the first failing command rather than blundering on.
    QVERIFY(s.contains(QStringLiteral("set -e")));
}

void SystemSetupTest::bluezScriptIsReversible() {
    const QString s = setup::privilegedFixScript(setup::CheckId::BluezBattery);
    QVERIFY(!s.isEmpty());
    // A drop-in leaves the distribution's own unit file untouched.
    QVERIFY(s.contains(QStringLiteral("/etc/systemd/system/bluetooth.service.d/10-sonero.conf")));
    QVERIFY(s.contains(QStringLiteral("--experimental")));
    QVERIFY(s.contains(QStringLiteral("systemctl daemon-reload")));
    // It must never rewrite the main BlueZ configuration in place.
    QVERIFY(!s.contains(QStringLiteral("/etc/bluetooth/main.conf")));
    QVERIFY(s.contains(QStringLiteral("set -e")));
}

void SystemSetupTest::noScriptForNonPrivilegedItems() {
    QVERIFY(setup::privilegedFixScript(setup::CheckId::PipeWire).isEmpty());
    QVERIFY(setup::privilegedFixScript(setup::CheckId::DesktopIntegration).isEmpty());
}

void SystemSetupTest::appImageDetectionFollowsEnvironment() {
    qunsetenv("APPIMAGE");
    QVERIFY(!setup::runningFromAppImage());
    qputenv("APPIMAGE", "/tmp/Some.AppImage");
    QVERIFY(setup::runningFromAppImage());
    qunsetenv("APPIMAGE");
}

void SystemSetupTest::checksAlwaysReportCoreItems() {
    const std::vector<setup::Check> checks = setup::runChecks();
    QVERIFY(!checks.empty());

    bool sawPipeWire = false, sawUdev = false;
    for (const setup::Check& c : checks) {
        QVERIFY(!c.title.isEmpty());
        QVERIFY(!c.detail.isEmpty());  // every row must explain itself to the user
        if (c.id == setup::CheckId::PipeWire) {
            sawPipeWire = true;
            QVERIFY(!c.fixable);  // installing an audio server is not our job
        }
        if (c.id == setup::CheckId::UdevRule) {
            sawUdev = true;
            QVERIFY(c.needsRoot);
        }
    }
    QVERIFY(sawPipeWire);
    QVERIFY(sawUdev);
}

// --- Superseded-bundle cleanup ----------------------------------------------
// It deletes a file the user downloaded, so every guard gets a case here.

namespace {
QTemporaryDir* scratch() {
    static QTemporaryDir dir;
    return &dir;
}
}  // namespace

QString SystemSetupTest::scratchDir() { return scratch()->path(); }

QString SystemSetupTest::makeBundle(const QString& name) {
    const QString path = scratchDir() + QLatin1Char('/') + name;
    QFile f(path);
    [[maybe_unused]] const bool opened = f.open(QIODevice::WriteOnly);
    Q_ASSERT(opened);
    f.write("not really an ELF, but a real file");
    f.close();
    return path;
}

void SystemSetupTest::writeMenuEntryPointingAt(const QString& program) {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
        QStringLiteral("/applications");
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/Sonero.desktop"));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=Sonero\n"
                           "Exec=\"%1\" %%U\n")
                .arg(program)
                .toUtf8());
}

void SystemSetupTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);  // keep ~/.local/share untouched
    QVERIFY(scratch()->isValid());
}

void SystemSetupTest::init() { qunsetenv("APPIMAGE"); }

void SystemSetupTest::removesTheBundleThatRanBefore() {
    const QString old = makeBundle(QStringLiteral("Sonero-0.1.1-x86_64.AppImage"));
    const QString now = makeBundle(QStringLiteral("Sonero-0.1.2-x86_64.AppImage"));
    writeMenuEntryPointingAt(old);
    qputenv("APPIMAGE", now.toUtf8());

    QCOMPARE(setup::removeSupersededBundle(setup::previousBundlePath()), old);
    QVERIFY(!QFileInfo::exists(old));
    QVERIFY(QFileInfo::exists(now));  // the running bundle survives
}

void SystemSetupTest::keepsTheRunningBundle() {
    const QString now = makeBundle(QStringLiteral("Sonero-same-x86_64.AppImage"));
    writeMenuEntryPointingAt(now);
    qputenv("APPIMAGE", now.toUtf8());

    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo::exists(now));
}

// A symlink to the running bundle is not an old bundle.
void SystemSetupTest::keepsABundleReachedBySymlink() {
    const QString real = makeBundle(QStringLiteral("Sonero-real-x86_64.AppImage"));
    const QString link = scratchDir() + QStringLiteral("/Sonero-link-x86_64.AppImage");
    QVERIFY(QFile::link(real, link));
    writeMenuEntryPointingAt(link);
    qputenv("APPIMAGE", real.toUtf8());

    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo::exists(real));
}

// The dangerous shape: a plain path, no symlink anywhere, that still names the
// running bundle. Only comparing resolved paths catches this — a string compare
// would delete the file out from under the process.
void SystemSetupTest::keepsABundleReachedByAnUncanonicalPath() {
    const QString real = makeBundle(QStringLiteral("Sonero-plain-x86_64.AppImage"));
    QVERIFY(QDir().mkpath(scratchDir() + QStringLiteral("/sub")));
    const QString detour =
        scratchDir() + QStringLiteral("/sub/../Sonero-plain-x86_64.AppImage");
    QVERIFY(!QFileInfo(detour).isSymLink());
    QVERIFY(QFileInfo(detour).isFile());

    writeMenuEntryPointingAt(detour);
    qputenv("APPIMAGE", real.toUtf8());

    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo::exists(real));
}

void SystemSetupTest::refusesAnythingThatIsNotABundleFile() {
    const QString now = makeBundle(QStringLiteral("Sonero-0.1.3-x86_64.AppImage"));
    qputenv("APPIMAGE", now.toUtf8());

    // An installed binary: a real file, but not a bundle.
    const QString installed = makeBundle(QStringLiteral("Sonero"));
    writeMenuEntryPointingAt(installed);
    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo::exists(installed));

    // A directory that happens to be named like one.
    const QString dir = scratchDir() + QStringLiteral("/Decoy.AppImage");
    QVERIFY(QDir().mkpath(dir));
    writeMenuEntryPointingAt(dir);
    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo(dir).isDir());
}

void SystemSetupTest::doesNothingWhenNotRunningFromABundle() {
    const QString old = makeBundle(QStringLiteral("Sonero-0.0.9-x86_64.AppImage"));
    writeMenuEntryPointingAt(old);
    qunsetenv("APPIMAGE");  // e.g. the .deb build, or a source-tree run

    QVERIFY(setup::removeSupersededBundle(setup::previousBundlePath()).isEmpty());
    QVERIFY(QFileInfo::exists(old));
}

// The path has to be read before the entry is rewritten, so it is its own step.
void SystemSetupTest::readsThePreviousBundleFromTheMenuEntry() {
    QVERIFY(setup::previousBundlePath().isEmpty() ||
            !setup::previousBundlePath().isEmpty());  // no entry yet: must not crash

    const QString bundle = makeBundle(QStringLiteral("Sonero-0.2.0-x86_64.AppImage"));
    writeMenuEntryPointingAt(bundle);
    QCOMPARE(setup::previousBundlePath(), bundle);
}

QTEST_GUILESS_MAIN(SystemSetupTest)
#include "SystemSetupTest.moc"
