#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "app/AutoStart.h"

namespace autostart = sonar::autostart;

class AutoStartTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void disabledWhenNoFile();
    void enableCreatesValidEntry();
    void disableRemovesEntry();
    void enableIsIdempotent();
    void refreshDoesNothingWhenDisabled();
    void refreshLeavesACurrentEntryAlone();
    void refreshRewritesWhenTheRecordedProgramIsGone();
    void refreshWillNotHijackAnEntryThatStillWorks();

private:
    // Writes an entry whose Exec points at `program`, as an older run would have.
    static void writeEntryPointingAt(const QString& program);
};

void AutoStartTest::writeEntryPointingAt(const QString& program) {
    QDir().mkpath(QFileInfo(autostart::desktopFilePath()).absolutePath());
    QFile f(autostart::desktopFilePath());
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(QStringLiteral("[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=Sonero\n"
                           "Exec=\"%1\" --background\n")
                .arg(program)
                .toUtf8());
}

void AutoStartTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);  // never touch the real autostart dir
    QCoreApplication::setOrganizationName(QStringLiteral("SoneroTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SoneroTest"));
}

void AutoStartTest::init() { QFile::remove(autostart::desktopFilePath()); }

void AutoStartTest::disabledWhenNoFile() {
    QVERIFY(!autostart::isEnabled());
    QVERIFY(autostart::desktopFilePath().endsWith(QStringLiteral("/autostart/Sonero.desktop")));
}

void AutoStartTest::enableCreatesValidEntry() {
    QVERIFY(autostart::setEnabled(true));
    QVERIFY(autostart::isEnabled());

    QFile f(autostart::desktopFilePath());
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(f.readAll());

    QVERIFY(text.startsWith(QStringLiteral("[Desktop Entry]")));
    QVERIFY(text.contains(QStringLiteral("Type=Application")));
    QVERIFY(text.contains(QStringLiteral("Name=Sonero")));
    // Must launch hidden, otherwise every login pops a window.
    QVERIFY(text.contains(QStringLiteral("--background")));
    // The Exec line must point at a real, absolute program path.
    const QString exec = autostart::executablePath();
    QVERIFY(!exec.isEmpty());
    QVERIFY(QFileInfo(exec).isAbsolute());
    QVERIFY(text.contains(exec));
}

void AutoStartTest::disableRemovesEntry() {
    QVERIFY(autostart::setEnabled(true));
    QVERIFY(autostart::isEnabled());
    QVERIFY(autostart::setEnabled(false));
    QVERIFY(!autostart::isEnabled());
    // Disabling again on an already-absent file is not an error.
    QVERIFY(autostart::setEnabled(false));
}

void AutoStartTest::enableIsIdempotent() {
    QVERIFY(autostart::setEnabled(true));
    const QString first = autostart::desktopFilePath();
    QVERIFY(autostart::setEnabled(true));  // rewriting must not fail or duplicate
    QCOMPARE(autostart::desktopFilePath(), first);
    QVERIFY(autostart::isEnabled());
}

// Autostart off: nothing to keep in step, and nothing may be created.
void AutoStartTest::refreshDoesNothingWhenDisabled() {
    QVERIFY(!autostart::isEnabled());
    QVERIFY(!autostart::refreshExecPath());
    QVERIFY(!autostart::isEnabled());
}

void AutoStartTest::refreshLeavesACurrentEntryAlone() {
    QVERIFY(autostart::setEnabled(true));
    QVERIFY(!autostart::refreshExecPath());  // already points here
}

// The AppImage case: the bundle the entry was switched on with is gone, so the
// entry is dead and this run takes it over.
void AutoStartTest::refreshRewritesWhenTheRecordedProgramIsGone() {
    writeEntryPointingAt(QStringLiteral("/nonexistent/Sonero-0.0.1-x86_64.AppImage"));
    QVERIFY(autostart::refreshExecPath());

    QFile f(autostart::desktopFilePath());
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(f.readAll());
    QVERIFY(text.contains(autostart::executablePath()));
    QVERIFY(!text.contains(QStringLiteral("/nonexistent/")));
    QVERIFY(text.contains(QStringLiteral("--background")));
}

// A source-tree run must not steal the login entry from a working install.
void AutoStartTest::refreshWillNotHijackAnEntryThatStillWorks() {
    QVERIFY(qEnvironmentVariable("APPIMAGE").isEmpty());  // not an AppImage run
    writeEntryPointingAt(QStringLiteral("/bin/sh"));      // a program that exists
    QVERIFY(!autostart::refreshExecPath());

    QFile f(autostart::desktopFilePath());
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(QString::fromUtf8(f.readAll()).contains(QStringLiteral("/bin/sh")));
}

QTEST_GUILESS_MAIN(AutoStartTest)
#include "AutoStartTest.moc"
