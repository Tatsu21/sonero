#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QStandardPaths>

#include "config/SettingsStore.h"
#include "dsp/Equalizer.h"

namespace dsp = sonar::dsp;
using sonar::config::eqFromJson;
using sonar::config::eqToJson;
using sonar::config::SettingsStore;

class SettingsStoreTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();  // fresh file before each test
    void firstRunWhenNoFile();
    void sectionSurvivesReopen();
    void eqRoundTrips();
    void damagedFileIsFirstRun();
};

void SettingsStoreTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);  // never touch the real user config
    QCoreApplication::setOrganizationName(QStringLiteral("LinuxSonarTest"));
    QCoreApplication::setApplicationName(QStringLiteral("LinuxSonarTest"));
}

void SettingsStoreTest::init() { QFile::remove(SettingsStore::filePath()); }

void SettingsStoreTest::firstRunWhenNoFile() {
    SettingsStore store;
    QVERIFY(store.isFirstRun());
    QVERIFY(store.section(QStringLiteral("mixer")).isEmpty());
}

void SettingsStoreTest::sectionSurvivesReopen() {
    {
        SettingsStore store;
        QJsonObject game;
        game[QStringLiteral("volume")] = 0.42;
        game[QStringLiteral("muted")] = true;
        QJsonObject mixer;
        mixer[QStringLiteral("Game")] = game;
        store.putSection(QStringLiteral("mixer"), mixer);
        store.flush();  // force the debounced write now
    }
    SettingsStore reopened;
    QVERIFY(!reopened.isFirstRun());
    const QJsonObject game = reopened.section(QStringLiteral("mixer"))
                                 .value(QStringLiteral("Game"))
                                 .toObject();
    QCOMPARE(game.value(QStringLiteral("volume")).toDouble(), 0.42);
    QCOMPARE(game.value(QStringLiteral("muted")).toBool(), true);
}

void SettingsStoreTest::eqRoundTrips() {
    dsp::EqSettings s;
    dsp::resetBands(s, dsp::BandCount::Bands15);
    s.enabled = true;
    s.preset = dsp::EqPreset::Custom;
    s.bands[3].gainDb = 6.5f;
    s.bands[10].gainDb = -4.0f;

    const dsp::EqSettings back = eqFromJson(eqToJson(s), dsp::EqSettings{});
    QCOMPARE(back.enabled, s.enabled);
    QCOMPARE(back.preset, s.preset);
    QCOMPARE(back.bandCount, dsp::BandCount::Bands15);
    QCOMPARE(back.bands.size(), s.bands.size());
    QCOMPARE(back.bands[3].gainDb, 6.5f);
    QCOMPARE(back.bands[10].gainDb, -4.0f);

    // An object with no bands returns the fallback untouched.
    const dsp::EqSettings fb = eqFromJson(QJsonObject{}, s);
    QCOMPARE(fb.bands.size(), s.bands.size());
}

void SettingsStoreTest::damagedFileIsFirstRun() {
    QFile f(SettingsStore::filePath());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not valid json ");
    f.close();
    SettingsStore store;
    QVERIFY(store.isFirstRun());  // unparseable => treated as a fresh start
}

QTEST_GUILESS_MAIN(SettingsStoreTest)
#include "SettingsStoreTest.moc"
