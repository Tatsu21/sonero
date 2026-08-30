#include "app/AutoStart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace sonar::autostart {

namespace {
constexpr char kEntryName[] = "Sonero.desktop";

QString autostartDir() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/autostart");
}

// The program path inside the entry's Exec line, unquoted. Empty when the line is
// missing or not in the quoted form setEnabled() writes.
QString recordedExecPath(const QString& text) {
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("Exec=\""))) {
            continue;
        }
        const qsizetype end = line.indexOf(QLatin1Char('"'), 6);
        if (end < 0) {
            return {};
        }
        QString path = line.mid(6, end - 6);
        path.replace(QLatin1String("\\\""), QLatin1String("\""));
        return path;
    }
    return {};
}
}  // namespace

QString desktopFilePath() {
    return autostartDir() + QLatin1Char('/') + QLatin1String(kEntryName);
}

QString executablePath() {
    // Inside an AppImage the running binary lives in a temporary mount that is gone
    // on the next boot; $APPIMAGE is the stable path of the bundle itself.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFileInfo::exists(appImage)) {
        return appImage;
    }
    return QCoreApplication::applicationFilePath();
}

bool isEnabled() { return QFileInfo::exists(desktopFilePath()); }

bool setEnabled(bool on) {
    const QString path = desktopFilePath();
    if (!on) {
        return !QFileInfo::exists(path) || QFile::remove(path);
    }

    if (!QDir().mkpath(autostartDir())) {
        return false;
    }
    // Quote the path so directories with spaces survive; Exec uses double quotes
    // per the Desktop Entry spec, with backslash-escaped inner quotes.
    QString exec = executablePath();
    exec.replace(QLatin1Char('"'), QLatin1String("\\\""));

    const QString contents =
        QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Sonero\n"
            "Comment=Audio mixer and router\n"
            "Exec=\"%1\" --background\n"
            "Icon=Sonero\n"
            "Terminal=false\n"
            "Categories=AudioVideo;Audio;Mixer;\n"
            "X-GNOME-Autostart-enabled=true\n")
            .arg(exec);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(contents.toUtf8());
    return file.commit();
}

QString execPathIn(const QString& desktopFile) {
    QFile file(desktopFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return recordedExecPath(QString::fromUtf8(file.readAll()));
}

bool refreshExecPath() {
    const QString recorded = execPathIn(desktopFilePath());
    if (recorded.isEmpty() || recorded == executablePath()) {
        return false;
    }
    // Only a run that is authoritative may take the entry over: an AppImage that
    // has just replaced an older bundle, or any build when the recorded program
    // is gone. See the header for why a source-tree run must not.
    const bool fromAppImage = !qEnvironmentVariable("APPIMAGE").isEmpty();
    if (!fromAppImage && QFileInfo::exists(recorded)) {
        return false;
    }
    return setEnabled(true);
}

}  // namespace sonar::autostart
