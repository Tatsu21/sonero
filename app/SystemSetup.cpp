#include "app/SystemSetup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include "app/AutoStart.h"

namespace sonar::setup {

namespace {

constexpr char kUdevRuleName[] = "70-sonero-steelseries.rules";
constexpr char kDesktopName[] = "Sonero.desktop";
const int kIconSizes[] = {32, 48, 64, 128, 256};
const int kTrayIconSizes[] = {16, 22, 24, 32, 48};

QString localShare() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

QString desktopEntryPath() {
    return localShare() + QStringLiteral("/applications/") + QLatin1String(kDesktopName);
}

bool udevRuleInstalled() {
    for (const char* dir : {"/etc/udev/rules.d/", "/usr/lib/udev/rules.d/",
                            "/lib/udev/rules.d/"}) {
        if (QFileInfo::exists(QLatin1String(dir) + QLatin1String(kUdevRuleName))) {
            return true;
        }
    }
    return false;
}

bool pipeWireRunning() {
    // The session socket is the most reliable, dependency-free signal.
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return !runtime.isEmpty() && QFileInfo::exists(runtime + QStringLiteral("/pipewire-0"));
}

// BlueZ only reports battery level with its experimental interfaces enabled,
// either in main.conf or via --experimental on the daemon's command line.
bool bluezExperimentalEnabled() {
    QFile conf(QStringLiteral("/etc/bluetooth/main.conf"));
    if (conf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray& raw : conf.readAll().split('\n')) {
            const QByteArray line = raw.trimmed();
            if (line.startsWith('#') || !line.startsWith("Experimental")) {
                continue;
            }
            if (line.toLower().contains("true")) {
                return true;
            }
        }
    }
    QProcess p;
    p.start(QStringLiteral("systemctl"),
            {QStringLiteral("show"), QStringLiteral("bluetooth"), QStringLiteral("-p"),
             QStringLiteral("ExecStart")});
    if (p.waitForFinished(1500)) {
        return QString::fromUtf8(p.readAllStandardOutput()).contains(QStringLiteral("--experimental"));
    }
    return false;
}

// Path of the bluetoothd binary, needed to write a systemd drop-in that keeps
// the original command but appends --experimental.
QString bluetoothDaemonPath() {
    QProcess p;
    p.start(QStringLiteral("systemctl"),
            {QStringLiteral("show"), QStringLiteral("bluetooth"), QStringLiteral("-p"),
             QStringLiteral("ExecStart")});
    if (p.waitForFinished(1500)) {
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        const int at = out.indexOf(QStringLiteral("path="));
        if (at >= 0) {
            const int end = out.indexOf(QLatin1Char(';'), at);
            return out.mid(at + 5, end < 0 ? -1 : end - at - 5).trimmed();
        }
    }
    for (const char* candidate : {"/usr/libexec/bluetooth/bluetoothd",
                                  "/usr/lib/bluetooth/bluetoothd"}) {
        if (QFileInfo::exists(QLatin1String(candidate))) {
            return QLatin1String(candidate);
        }
    }
    return QStringLiteral("/usr/libexec/bluetooth/bluetoothd");
}

// Shell-quote a path for safe embedding in the privileged script.
QString shq(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

}  // namespace

bool runningFromAppImage() { return !qEnvironmentVariable("APPIMAGE").isEmpty(); }

QString resourcePath(const QString& relative) {
    QStringList roots;
    const QString appDir = qEnvironmentVariable("APPDIR");
    if (!appDir.isEmpty()) {
        roots << appDir + QStringLiteral("/usr/share/sonero");
        roots << appDir + QStringLiteral("/usr/share");
    }
    roots << QStringLiteral("/usr/share/sonero") << QStringLiteral("/usr/local/share/sonero");
    // Development: run straight from the build tree.
    roots << QCoreApplication::applicationDirPath() + QStringLiteral("/../packaging");
    roots << QStringLiteral(SONAR_SOURCE_DIR "/packaging");

    for (const QString& root : roots) {
        const QString candidate = root + QLatin1Char('/') + relative;
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

std::vector<Check> runChecks() {
    std::vector<Check> checks;

    Check pw;
    pw.id = CheckId::PipeWire;
    pw.title = QStringLiteral("PipeWire audio server");
    pw.status = pipeWireRunning() ? Status::Ok : Status::Missing;
    pw.detail = pw.status == Status::Ok
                    ? QStringLiteral("Running — Sonero's mixing and routing are active.")
                    : QStringLiteral("Not detected. Sonero needs PipeWire; install it with "
                                     "your distribution's package manager.");
    pw.fixable = false;  // installing an audio server is the distro's job
    checks.push_back(pw);

    Check desktop;
    desktop.id = CheckId::DesktopIntegration;
    desktop.title = QStringLiteral("Application menu entry");
    if (!runningFromAppImage()) {
        desktop.status = Status::NotNeeded;
        desktop.detail = QStringLiteral("Handled by your package manager for installed builds.");
    } else if (QFileInfo::exists(desktopEntryPath())) {
        desktop.status = Status::Ok;
        desktop.detail = QStringLiteral("Sonero appears in your app menu with its icon.");
    } else {
        desktop.status = Status::Missing;
        desktop.detail = QStringLiteral("Adds Sonero to your app menu and gives it an icon.");
        desktop.fixable = true;
    }
    checks.push_back(desktop);

    Check udev;
    udev.id = CheckId::UdevRule;
    udev.title = QStringLiteral("SteelSeries headset access");
    udev.status = udevRuleInstalled() ? Status::Ok : Status::Missing;
    udev.detail =
        udev.status == Status::Ok
            ? QStringLiteral("Battery level and the onboard equalizer are available.")
            : QStringLiteral("Needed to read battery and write the onboard EQ of SteelSeries "
                             "headsets. Installs a udev rule granting your user access.");
    udev.needsRoot = true;
    udev.fixable = udev.status == Status::Missing;
    checks.push_back(udev);

    Check bt;
    bt.id = CheckId::BluezBattery;
    bt.title = QStringLiteral("Bluetooth battery reporting");
    bt.status = bluezExperimentalEnabled() ? Status::Ok : Status::Missing;
    bt.detail = bt.status == Status::Ok
                    ? QStringLiteral("Battery level of Bluetooth headsets is shown.")
                    : QStringLiteral("Enables BlueZ's battery interface. Restarts the Bluetooth "
                                     "service, which briefly disconnects paired devices.");
    bt.needsRoot = true;
    bt.fixable = bt.status == Status::Missing;
    checks.push_back(bt);

    return checks;
}

bool installDesktopIntegration() {
    // Only meaningful for a portable bundle; installed builds ship these files.
    if (!runningFromAppImage()) {
        return true;
    }

    const QString appsDir = localShare() + QStringLiteral("/applications");
    if (!QDir().mkpath(appsDir)) {
        return false;
    }

    QString exec = autostart::executablePath();
    exec.replace(QLatin1Char('"'), QLatin1String("\\\""));
    const QString entry =
        QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Sonero\n"
            "GenericName=Audio Mixer\n"
            "Comment=Per-application audio mixer, router and equalizer for PipeWire\n"
            "Exec=\"%1\" %U\n"
            "Icon=Sonero\n"
            "Terminal=false\n"
            "Categories=AudioVideo;Audio;Mixer;\n"
            "Keywords=audio;mixer;equalizer;pipewire;sonar;volume;\n"
            "StartupNotify=true\n"
            "StartupWMClass=Sonero\n")
            .arg(exec);

    QSaveFile file(desktopEntryPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(entry.toUtf8());
    if (!file.commit()) {
        return false;
    }

    // Icons, so the menu entry, window and notifications all show the app icon —
    // plus the separate panel icon the system tray looks up by name.
    const auto installIcon = [](const QString& src, int size, const QString& name) {
        if (src.isEmpty()) {
            return;
        }
        const QString dstDir = QStringLiteral("%1/icons/hicolor/%2x%2/apps")
                                   .arg(localShare())
                                   .arg(size);
        QDir().mkpath(dstDir);
        const QString dst = dstDir + QLatin1Char('/') + name;
        QFile::remove(dst);  // QFile::copy refuses to overwrite
        QFile::copy(src, dst);
    };
    for (const int size : kIconSizes) {
        installIcon(resourcePath(QStringLiteral("icons/sonero-%1.png").arg(size)), size,
                    QStringLiteral("Sonero.png"));
    }
    for (const int size : kTrayIconSizes) {
        installIcon(resourcePath(QStringLiteral("icons/sonero-tray-%1.png").arg(size)), size,
                    QStringLiteral("Sonero-tray.png"));
    }

    // Best-effort cache refresh; desktops that watch the directory don't need it.
    QProcess::startDetached(QStringLiteral("update-desktop-database"), {appsDir});
    QProcess::startDetached(QStringLiteral("gtk-update-icon-cache"),
                            {QStringLiteral("-qtf"),
                             localShare() + QStringLiteral("/icons/hicolor")});
    return QFileInfo::exists(desktopEntryPath());
}

QString privilegedFixScript(CheckId id) {
    switch (id) {
        case CheckId::UdevRule: {
            const QString rule = resourcePath(QStringLiteral("udev/") + QLatin1String(kUdevRuleName));
            if (rule.isEmpty()) {
                return {};
            }
            return QStringLiteral(
                       "#!/bin/sh\n"
                       "set -e\n"
                       "install -m 0644 %1 /etc/udev/rules.d/%2\n"
                       "udevadm control --reload-rules\n"
                       "udevadm trigger --subsystem-match=hidraw\n")
                .arg(shq(rule), QLatin1String(kUdevRuleName));
        }
        case CheckId::BluezBattery: {
            // A systemd drop-in keeps the distro's own unit intact and is trivial
            // to undo (delete the file), unlike editing main.conf in place.
            return QStringLiteral(
                       "#!/bin/sh\n"
                       "set -e\n"
                       "mkdir -p /etc/systemd/system/bluetooth.service.d\n"
                       "cat > /etc/systemd/system/bluetooth.service.d/10-sonero.conf <<'EOF'\n"
                       "[Service]\n"
                       "ExecStart=\n"
                       "ExecStart=%1 --experimental\n"
                       "EOF\n"
                       "systemctl daemon-reload\n"
                       "systemctl restart bluetooth\n")
                .arg(bluetoothDaemonPath());
        }
        case CheckId::PipeWire:
        case CheckId::DesktopIntegration:
            return {};
    }
    return {};
}

bool runPrivilegedFix(CheckId id, QString* error) {
    const QString script = privilegedFixScript(id);
    if (script.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("No system change is available for this item.");
        }
        return false;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "pkexec (PolicyKit) is not installed, so Sonero cannot ask for "
                "administrator rights. Run the shown commands manually with sudo.");
        }
        return false;
    }

    // Write the script somewhere root can read, then run it through pkexec so the
    // desktop shows its standard authentication dialog.
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("sonero-setup.sh"));
    {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not write the setup script.");
            }
            return false;
        }
        f.write(script.toUtf8());
        if (!f.commit()) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not write the setup script.");
            }
            return false;
        }
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                    QFile::ReadGroup | QFile::ReadOther);

    QProcess proc;
    proc.start(QStringLiteral("pkexec"), {QStringLiteral("/bin/sh"), path});
    if (!proc.waitForFinished(120000)) {
        QFile::remove(path);
        if (error != nullptr) {
            *error = QStringLiteral("The setup command timed out.");
        }
        return false;
    }
    const int code = proc.exitCode();
    const QString stderrText = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    QFile::remove(path);

    if (code == 0) {
        return true;
    }
    if (error != nullptr) {
        // 126 = authentication dialog dismissed, 127 = pkexec could not run it.
        *error = code == 126
                     ? QStringLiteral("Cancelled — nothing was changed.")
                     : (stderrText.isEmpty()
                            ? QStringLiteral("The setup command failed (exit %1).").arg(code)
                            : stderrText);
    }
    return false;
}

}  // namespace sonar::setup
