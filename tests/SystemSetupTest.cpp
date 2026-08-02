#include <QtTest>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include "app/SystemSetup.h"

namespace setup = sonar::setup;

class SystemSetupTest : public QObject {
    Q_OBJECT

private slots:
    void findsPackagedResources();
    void udevScriptIsExactAndScoped();
    void bluezScriptIsReversible();
    void noScriptForNonPrivilegedItems();
    void appImageDetectionFollowsEnvironment();
    void checksAlwaysReportCoreItems();
};

void SystemSetupTest::findsPackagedResources() {
    // The udev rule must be locatable, otherwise the fix silently does nothing.
    const QString rule =
        setup::resourcePath(QStringLiteral("udev/70-linuxsonar-steelseries.rules"));
    QVERIFY2(!rule.isEmpty(), "packaged udev rule not found");
    QVERIFY(QFileInfo::exists(rule));

    QVERIFY(!setup::resourcePath(QStringLiteral("icons/linuxsonar-256.png")).isEmpty());
    QVERIFY(setup::resourcePath(QStringLiteral("nope/does-not-exist")).isEmpty());
}

void SystemSetupTest::udevScriptIsExactAndScoped() {
    const QString s = setup::privilegedFixScript(setup::CheckId::UdevRule);
    QVERIFY(!s.isEmpty());

    // Installs the rule read-only and reloads udev — nothing else.
    QVERIFY(s.contains(QStringLiteral("install -m 0644")));
    QVERIFY(s.contains(QStringLiteral("/etc/udev/rules.d/70-linuxsonar-steelseries.rules")));
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
    QVERIFY(s.contains(QStringLiteral("/etc/systemd/system/bluetooth.service.d/10-linuxsonar.conf")));
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

QTEST_GUILESS_MAIN(SystemSetupTest)
#include "SystemSetupTest.moc"
