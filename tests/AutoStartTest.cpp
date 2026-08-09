#include <QtTest>

#include <QCoreApplication>
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
};

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

QTEST_GUILESS_MAIN(AutoStartTest)
#include "AutoStartTest.moc"
