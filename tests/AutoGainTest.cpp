#include <QtTest>

#include <cmath>

#include "dsp/AutoGain.h"

using sonar::dsp::AutoGain;

class AutoGainTest : public QObject {
    Q_OBJECT

private slots:
    void observesBeforeItDecides();
    void settlesOnAConstantGain();
    void staysConstantThroughLoudPassages();
    void reevaluatesWhenTheMaterialReallyChanges();
    void ducksImmediatelyNearFullScale();
    void holdsStillDuringSilence();
    void staysWithinItsLimits();
    void survivesJunkInput();

private:
    // Mirror the real path: the meter reads the channel output, so what it reports
    // already includes the gain the loop applied.
    static float run(AutoGain& agc, float sourcePeak, float seconds, float dt = 0.04f) {
        const int steps = static_cast<int>(seconds / dt);
        for (int i = 0; i < steps; ++i) {
            agc.update(sourcePeak * std::pow(10.0f, agc.gainDb() / 20.0f), dt);
        }
        return agc.gainDb();
    }
};

void AutoGainTest::observesBeforeItDecides() {
    AutoGain agc;
    // Material below the limiter ceiling, so only the observe/hold logic is in
    // play: it must not lunge at the first buffer.
    run(agc, 0.5f, 0.5f);
    QCOMPARE(agc.gainDb(), 0.0f);
    QVERIFY(!agc.settled());

    run(agc, 0.5f, 3.0f);
    QVERIFY(agc.settled());
}

void AutoGainTest::settlesOnAConstantGain() {
    AutoGain agc;
    const float g = run(agc, 1.0f, 8.0f);
    QVERIFY2(g < -1.5f && g > -6.0f, qPrintable(QStringLiteral("gain %1").arg(g)));

    // Once settled the gain must be exactly constant — not merely stable-ish. A
    // gain that keeps inching is what makes normalisation audible.
    const float settled = agc.gainDb();
    run(agc, 1.0f, 10.0f);
    QCOMPARE(agc.gainDb(), settled);
}

void AutoGainTest::staysConstantThroughLoudPassages() {
    AutoGain agc;
    run(agc, 0.85f, 8.0f);  // settle: needs a small cut to reach the target
    const float settled = agc.gainDb();
    QVERIFY(settled < 0.0f);

    // A chorus a little louder for two seconds — still short of the limiter
    // ceiling, and well inside the re-evaluation threshold.
    run(agc, 0.95f, 2.0f);
    QCOMPARE(agc.gainDb(), settled);

    // And back to the verse without having moved at all.
    run(agc, 0.85f, 2.0f);
    QCOMPARE(agc.gainDb(), settled);
}

void AutoGainTest::reevaluatesWhenTheMaterialReallyChanges() {
    AutoGain agc;
    run(agc, 0.1f, 8.0f);  // a quiet track gets lifted, up to the ceiling
    const float quietGain = agc.gainDb();
    QVERIFY(quietGain > 1.0f);

    // A much louder track, sustained: this should trigger a fresh decision.
    run(agc, 1.0f, 25.0f);
    QVERIFY2(agc.gainDb() < quietGain - 3.0f,
             qPrintable(QStringLiteral("%1 -> %2").arg(quietGain).arg(agc.gainDb())));
    QVERIFY(agc.settled());
}

void AutoGainTest::ducksImmediatelyNearFullScale() {
    AutoGain agc;
    run(agc, 0.3f, 8.0f);  // settled, plenty of gain in hand
    const float before = agc.gainDb();

    // One buffer at full scale: the limiter must act now, not after observing.
    agc.update(1.0f, 0.04f);
    QVERIFY2(agc.gainDb() < before,
             qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(agc.gainDb())));
}

void AutoGainTest::holdsStillDuringSilence() {
    AutoGain agc;
    run(agc, 1.0f, 8.0f);
    const float before = agc.gainDb();
    for (int i = 0; i < 500; ++i) {
        agc.update(0.0f, 0.04f);
    }
    QCOMPARE(agc.gainDb(), before);
}

void AutoGainTest::staysWithinItsLimits() {
    AutoGain quiet;
    // However long it plays, boost stops at the ceiling.
    QVERIFY(run(quiet, 0.00001f, 90.0f) <= 6.0f);
    AutoGain loud;
    QVERIFY(run(loud, 50.0f, 90.0f) >= -20.0f);
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

QTEST_GUILESS_MAIN(AutoGainTest)
#include "AutoGainTest.moc"
