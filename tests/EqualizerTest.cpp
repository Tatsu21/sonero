#include <QtTest>

#include <cmath>

#include "dsp/Equalizer.h"

namespace dsp = sonar::dsp;

class EqualizerTest : public QObject {
    Q_OBJECT

private slots:
    void bandGainMatchesTheBandItSitsOn();
    void bandGainInterpolatesBetweenBands();
    void bandGainDoesNotSumNeighbours();
    void peakBoostIsZeroWhenFlatOrDisabled();
    void peakBoostCoversTheWholeCurve();
    void headroomKeepsBoostedOutputAtUnity();

private:
    // The user-reported case: a bass-and-treble curve on a 10-band EQ.
    static dsp::EqSettings musicPreset();
};

dsp::EqSettings EqualizerTest::musicPreset() {
    dsp::EqSettings s;
    s.enabled = true;
    s.bandCount = dsp::BandCount::Bands10;
    const float f[10] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    const float g[10] = {6, 5, 2, 0, -2, 0, 1, 3, 4, 5};
    for (int i = 0; i < 10; ++i) {
        s.bands.push_back(dsp::EqBand{f[i], g[i]});
    }
    return s;
}

void EqualizerTest::bandGainMatchesTheBandItSitsOn() {
    const dsp::EqSettings s = musicPreset();
    // At a band's own centre frequency the filter must apply exactly that band's
    // gain — no more. This is the regression that caused audible clipping: the
    // summed display curve was being fed to every filter in the cascade.
    for (const dsp::EqBand& band : s.bands) {
        QCOMPARE(dsp::bandGainAt(s, band.frequency), band.gainDb);
    }
}

void EqualizerTest::bandGainInterpolatesBetweenBands() {
    const dsp::EqSettings s = musicPreset();
    // Halfway between 31 Hz (+6) and 62 Hz (+5) in log-frequency space.
    const float mid = std::sqrt(31.0f * 62.0f);
    const float g = dsp::bandGainAt(s, mid);
    QVERIFY(g < 6.0f && g > 5.0f);
    QVERIFY(std::abs(g - 5.5f) < 0.1f);

    // Outside the band range it holds the edge value rather than extrapolating.
    QCOMPARE(dsp::bandGainAt(s, 5.0f), 6.0f);
    QCOMPARE(dsp::bandGainAt(s, 30000.0f), 5.0f);
}

void EqualizerTest::bandGainDoesNotSumNeighbours() {
    // Two adjacent boosted bands: the summed curve exceeds either one, but the
    // per-filter gain must not.
    dsp::EqSettings s;
    s.enabled = true;
    s.bands = {{100.0f, 6.0f}, {200.0f, 6.0f}};

    QVERIFY(dsp::responseDbAt(s, 100.0f) > 6.0f);   // display curve sums
    QCOMPARE(dsp::bandGainAt(s, 100.0f), 6.0f);     // filter gain does not
}

void EqualizerTest::peakBoostIsZeroWhenFlatOrDisabled() {
    dsp::EqSettings flat;
    flat.enabled = true;
    dsp::resetBands(flat, dsp::BandCount::Bands10);
    QCOMPARE(dsp::peakBoostDb(flat), 0.0f);

    // A cut-only curve must not attenuate further: there is nothing to clip.
    dsp::EqSettings cuts;
    cuts.enabled = true;
    cuts.bands = {{100.0f, -6.0f}, {1000.0f, -3.0f}};
    QCOMPARE(dsp::peakBoostDb(cuts), 0.0f);

    // Disabled EQ means no headroom is taken away from the user's volume.
    dsp::EqSettings off = musicPreset();
    off.enabled = false;
    QCOMPARE(dsp::peakBoostDb(off), 0.0f);
}

void EqualizerTest::peakBoostCoversTheWholeCurve() {
    const dsp::EqSettings s = musicPreset();
    const float peak = dsp::peakBoostDb(s);

    // Overlapping boosted bands push the curve above the highest single band.
    QVERIFY(peak >= 6.0f);
    // Sanity: it must actually bound the curve everywhere we can sample it.
    for (float f = 20.0f; f < 20000.0f; f *= 1.05f) {
        QVERIFY(dsp::responseDbAt(s, f) <= peak + 0.01f);
    }
}

void EqualizerTest::headroomKeepsBoostedOutputAtUnity() {
    const dsp::EqSettings s = musicPreset();
    const float headroom = dsp::peakBoostDb(s);
    const float scale = std::pow(10.0f, -headroom / 20.0f);

    // Attenuating by the peak boost brings the loudest EQ'd frequency back to
    // unity, so a full-scale input cannot clip on conversion to the device format.
    QVERIFY(scale > 0.0f && scale <= 1.0f);
    for (float f = 20.0f; f < 20000.0f; f *= 1.05f) {
        const float linear = std::pow(10.0f, dsp::responseDbAt(s, f) / 20.0f);
        QVERIFY2(linear * scale <= 1.0001f, qPrintable(QStringLiteral("clips at %1 Hz").arg(f)));
    }
}

QTEST_GUILESS_MAIN(EqualizerTest)
#include "EqualizerTest.moc"
