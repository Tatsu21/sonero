#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QStandardPaths>

#include "config/SettingsStore.h"
#include "dsp/Equalizer.h"

namespace dsp = sonar::dsp;
using sonar::config::bassFromJson;
using sonar::config::bassToJson;
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
    void micRoundTrips();
    void micFromJsonFallsBackAndClamps();
    void bassRoundTripsAndFallsBack();
    void damagedFileIsFirstRun();
};

void SettingsStoreTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);  // never touch the real user config
    QCoreApplication::setOrganizationName(QStringLiteral("SoneroTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SoneroTest"));
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

void SettingsStoreTest::micRoundTrips() {
    dsp::MicSettings s = dsp::presetSettings(dsp::MicPreset::Streaming);
    s.inputGainDb = 7.0f;
    s.muted = true;
    s.gate.thresholdDb = -33.0f;
    s.gate.ratio = 2.5f;
    s.deEsser.enabled = false;
    s.compressor.autoMakeup = false;
    s.compressor.makeupDb = 4.0f;
    s.limiter.ceilingDb = -3.0f;

    const dsp::MicSettings back = sonar::config::micFromJson(sonar::config::micToJson(s),
                                                             dsp::MicSettings{});
    QCOMPARE(back.inputGainDb, 7.0f);
    QCOMPARE(back.muted, true);
    QCOMPARE(back.gate.thresholdDb, -33.0f);
    QCOMPARE(back.gate.ratio, 2.5f);
    QCOMPARE(back.deEsser.enabled, false);
    QCOMPARE(back.compressor.autoMakeup, false);
    QCOMPARE(back.compressor.makeupDb, 4.0f);
    QCOMPARE(back.limiter.ceilingDb, -3.0f);
    QCOMPARE(back.highPass.frequencyHz, s.highPass.frequencyHz);
}

void SettingsStoreTest::micFromJsonFallsBackAndClamps() {
    dsp::MicSettings fallback;
    fallback.gate.thresholdDb = -50.0f;

    // Files saved before the stages had controls carry none of them.
    QJsonObject old;
    old[QStringLiteral("inputGainDb")] = 3;
    dsp::MicSettings s = sonar::config::micFromJson(old, fallback);
    QCOMPARE(s.inputGainDb, 3.0f);
    QCOMPARE(s.gate.thresholdDb, -50.0f);

    // A ratio of zero would divide by zero on the audio thread.
    QJsonObject comp;
    comp[QStringLiteral("ratio")] = 0;
    QJsonObject edited;
    edited[QStringLiteral("compressor")] = comp;
    s = sonar::config::micFromJson(edited, fallback);
    QCOMPARE(s.compressor.ratio, 1.0f);
}

void SettingsStoreTest::bassRoundTripsAndFallsBack() {
    dsp::BassStageSettings b;
    b.bass.enabled = !b.bass.enabled;
    b.bass.crossoverHz = 95.0f;
    b.bass.amountDb = 7.0f;
    b.bass.harmonics = 3;
    b.bass.evenWeight = 0.25f;
    b.bass.attackMs = 4.0f;
    b.bass.releaseMs = 120.0f;
    b.bass.replaceLows = !b.bass.replaceLows;
    b.limiterEnabled = !b.limiterEnabled;
    b.limiterCeilingDb = -2.0f;

    const dsp::BassStageSettings back = bassFromJson(bassToJson(b), dsp::BassStageSettings{});
    QCOMPARE(back.bass.enabled, b.bass.enabled);
    QCOMPARE(back.bass.crossoverHz, b.bass.crossoverHz);
    QCOMPARE(back.bass.amountDb, b.bass.amountDb);
    QCOMPARE(back.bass.harmonics, b.bass.harmonics);
    QCOMPARE(back.bass.evenWeight, b.bass.evenWeight);
    QCOMPARE(back.bass.attackMs, b.bass.attackMs);
    QCOMPARE(back.bass.releaseMs, b.bass.releaseMs);
    QCOMPARE(back.bass.replaceLows, b.bass.replaceLows);
    QCOMPARE(back.limiterEnabled, b.limiterEnabled);
    QCOMPARE(back.limiterCeilingDb, b.limiterCeilingDb);

    // Missing and mistyped keys keep the fallback.
    QJsonObject partial;
    partial[QStringLiteral("amountDb")] = 3.0;
    partial[QStringLiteral("harmonics")] = QStringLiteral("many");
    const dsp::BassStageSettings merged = bassFromJson(partial, b);
    QCOMPARE(merged.bass.amountDb, 3.0f);
    QCOMPARE(merged.bass.harmonics, b.bass.harmonics);
    QCOMPARE(merged.bass.crossoverHz, b.bass.crossoverHz);
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
