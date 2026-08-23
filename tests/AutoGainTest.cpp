#include <QtTest>

#include <cmath>

#include "dsp/AutoGain.h"

using sonar::dsp::AutoGain;

// Auto-gain holds the largest gain, up to its ceiling, that keeps the channel's
// output under full scale: quiet material climbs to the top of the range, hot
// material is pulled down by exactly what it overshoots.
class AutoGainTest : public QObject {
    Q_OBJECT

private slots:
    void climbsToTheTopWhenThereIsRoom();
    void stopsWhereThePeakRunsOutOfRoom();
    void reducesByWhatTheOvershootDemands();
    void reactsToATransientImmediately();
    void doesNotClimbIntoTheGapsBetweenTransients();
    void forgetsTheOldPeaksAfterSilence();
    void leavesLoudMaterialWhereItIs();
    void keepsHeadroomForAPassageItHasNotHeardYet();
    void holdsStillDuringSilence();
    void staysWithinItsLimits();
    void survivesJunkInput();

private:
    // Mirror the real path: the meter reads the channel output, so what it reports
    // already includes the gain the loop applied. `rawOutputPeak` is what the
    // channel would produce at 0 dB — the material and the fader together.
    static float run(AutoGain& agc, float rawOutputPeak, float seconds, float dt = 0.04f) {
        const int steps = static_cast<int>(seconds / dt);
        for (int i = 0; i < steps; ++i) {
            agc.update(rawOutputPeak * std::pow(10.0f, agc.gainDb() / 20.0f), dt);
        }
        return agc.gainDb();
    }

    // The gain the climb aims for: the one putting `rawOutputPeak` on the target.
    static float roomFor(float rawOutputPeak) {
        return 20.0f * std::log10(0.5f / rawOutputPeak);
    }
    // Where a duck lands: the ceiling, minus the extra margin it takes.
    static float duckedTo(float rawOutputPeak) {
        return 20.0f * std::log10(0.89f / rawOutputPeak) - 1.0f;
    }
    static void silence(AutoGain& agc, float seconds, float dt = 0.04f) {
        for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
            agc.update(0.0f, dt);
        }
    }
};

void AutoGainTest::climbsToTheTopWhenThereIsRoom() {
    AutoGain agc;
    // Far too quiet to ever reach the ceiling: it must take everything on offer.
    QCOMPARE(run(agc, 0.05f, 60.0f), 6.0f);
}

void AutoGainTest::stopsWhereThePeakRunsOutOfRoom() {
    AutoGain agc;
    // Room for about +4.4 dB — under the +6 limit, so the peak decides, not the
    // limit. It must stop there rather than taking the last fraction.
    const float settled = run(agc, 0.3f, 60.0f);
    QVERIFY2(std::abs(settled - roomFor(0.3f)) < 0.3f,
             qPrintable(QStringLiteral("settled %1, room for %2")
                            .arg(settled)
                            .arg(roomFor(0.3f))));
}

void AutoGainTest::reducesByWhatTheOvershootDemands() {
    AutoGain agc;
    // Well past the ceiling: down by the overshoot plus the margin, not to the floor.
    const float settled = run(agc, 1.54f, 30.0f);
    QVERIFY2(std::abs(settled - duckedTo(1.54f)) < 0.4f,
             qPrintable(QStringLiteral("settled %1, expected %2")
                            .arg(settled)
                            .arg(duckedTo(1.54f))));
}

void AutoGainTest::reactsToATransientImmediately() {
    AutoGain agc;
    run(agc, 0.9f, 20.0f);
    const float before = agc.gainDb();
    agc.update(1.4f, 0.04f);  // one buffer past the ceiling
    QVERIFY2(agc.gainDb() < before,
             qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(agc.gainDb())));
}

void AutoGainTest::doesNotClimbIntoTheGapsBetweenTransients() {
    AutoGain agc;
    run(agc, 1.4f, 20.0f);  // settled where the transients allow
    const float settled = agc.gainDb();

    // Material that is mostly quiet with a peak every half second: the quiet parts
    // must not be read as room, or the gain would breathe with the beat.
    for (int i = 0; i < 750; ++i) {
        const float linear = std::pow(10.0f, agc.gainDb() / 20.0f);
        agc.update((i % 12 == 0 ? 1.4f : 0.2f) * linear, 0.04f);
    }
    QVERIFY2(std::abs(agc.gainDb() - settled) < 0.6f,
             qPrintable(QStringLiteral("%1 -> %2").arg(settled).arg(agc.gainDb())));
}

// A loud passage has to keep counting long after it ends — that is what stops the
// gain creeping back up during the quiet verse and clipping on the next chorus.
void AutoGainTest::forgetsTheOldPeaksAfterSilence() {
    AutoGain agc;
    const float ducked = run(agc, 2.0f, 20.0f);
    QVERIFY(ducked < 0.0f);

    // Quiet material straight after: the loud passage is still remembered, so the
    // climb stays modest rather than running back to the top.
    run(agc, 0.05f, 20.0f);
    QVERIFY2(agc.gainDb() < 6.0f, qPrintable(QStringLiteral("climbed to %1").arg(agc.gainDb())));

    // But once the track is actually over, the next one is judged on its own.
    silence(agc, 5.0f);
    QCOMPARE(run(agc, 0.05f, 60.0f), 6.0f);
}

// Loud material is left alone: this device does not normalise, so a master that is
// already hot neither climbs nor gets pulled down to some target.
void AutoGainTest::leavesLoudMaterialWhereItIs() {
    AutoGain agc;
    QCOMPARE(run(agc, 0.85f, 60.0f), 0.0f);
}

// The point of the whole design: after settling on quiet material, a passage that
// suddenly arrives 6 dB louder must still fit under full scale.
void AutoGainTest::keepsHeadroomForAPassageItHasNotHeardYet() {
    AutoGain agc;
    const float raw = 0.25f;
    const float settled = run(agc, raw, 60.0f);
    const float outputNow = raw * std::pow(10.0f, settled / 20.0f);
    QVERIFY2(outputNow * 2.0f <= 1.0f,
             qPrintable(QStringLiteral("output %1, doubles to %2")
                            .arg(outputNow)
                            .arg(outputNow * 2.0f)));
}

void AutoGainTest::holdsStillDuringSilence() {
    AutoGain agc;
    const float settled = run(agc, 1.5f, 20.0f);
    for (int i = 0; i < 500; ++i) {
        agc.update(0.0f, 0.04f);
    }
    QCOMPARE(agc.gainDb(), settled);
}

void AutoGainTest::staysWithinItsLimits() {
    AutoGain loud;
    QVERIFY(run(loud, 500.0f, 90.0f) >= -20.0f);
    AutoGain quiet;
    QVERIFY(run(quiet, 0.00001f, 90.0f) <= 6.0f);
}

void AutoGainTest::survivesJunkInput() {
    AutoGain agc;
    agc.update(-1.0f, 0.04f);
    agc.update(0.5f, 0.0f);
    agc.update(0.5f, -1.0f);
    agc.update(1e9f, 10.0f);
    QVERIFY(std::isfinite(agc.gainDb()));
    QVERIFY(agc.gainDb() >= -20.0f && agc.gainDb() <= 6.0f);
}

QTEST_MAIN(AutoGainTest)
#include "AutoGainTest.moc"
