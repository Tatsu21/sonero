#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/Limiter.h"

namespace dsp = sonar::dsp;

namespace {

constexpr float kSampleRate = 48000.0f;

std::vector<float> sine(float freqHz, float amplitude, int count) {
    std::vector<float> buf(static_cast<std::size_t>(count), 0.0f);
    const double w = 2.0 * std::numbers::pi * freqHz / kSampleRate;
    for (int n = 0; n < count; ++n) {
        buf[static_cast<std::size_t>(n)] = static_cast<float>(amplitude * std::sin(w * n));
    }
    return buf;
}

[[nodiscard]] float linOf(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace

class LimiterTest : public QObject {
    Q_OBJECT

private slots:
    void nothingLeavesAboveTheCeiling();
    void quietSignalIsUntouched();
    void disabledPassesThroughAndCountsClipping();
    void enabledNeverReportsClipping();
    void lookaheadCostsLatencyOnlyWhileEnabled();
    void silenceStaysSilent();
};

void LimiterTest::nothingLeavesAboveTheCeiling() {
    // The promise, against a signal 20 dB over it.
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    limiter.setCeilingDb(-1.0f);
    limiter.setEnabled(true);

    constexpr int kCount = 48000;
    const std::vector<float> in = sine(1000.0f, 10.0f, kCount);
    std::vector<float> out(static_cast<std::size_t>(kCount), 0.0f);
    limiter.process(in.data(), out.data(), kCount);

    const float ceiling = linOf(-1.0f);
    for (const float v : out) {
        QVERIFY2(std::abs(v) <= ceiling + 1e-5f,
                 qPrintable(QStringLiteral("sample past the ceiling: %1").arg(v)));
    }
    QVERIFY2(limiter.reductionDb() < -15.0f,
             qPrintable(QStringLiteral("barely engaged: %1 dB").arg(limiter.reductionDb())));
}

void LimiterTest::quietSignalIsUntouched() {
    // A limiter that colours material which never approaches the ceiling is not
    // a limiter, it is a compressor nobody asked for.
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    limiter.setCeilingDb(-1.0f);
    limiter.setEnabled(true);

    constexpr int kCount = 24000;
    const std::vector<float> in = sine(1000.0f, 0.2f, kCount);
    std::vector<float> out(static_cast<std::size_t>(kCount), 0.0f);
    limiter.process(in.data(), out.data(), kCount);

    // Delayed by the lookahead, so compare past it rather than sample for sample.
    const int look = limiter.lookaheadSamples();
    for (int i = look; i < kCount; ++i) {
        const float expected = in[static_cast<std::size_t>(i - look)];
        QVERIFY2(std::abs(out[static_cast<std::size_t>(i)] - expected) < 1e-6f,
                 "a quiet signal was altered");
    }
    QCOMPARE(limiter.reductionDb(), 0.0f);
}

void LimiterTest::disabledPassesThroughAndCountsClipping() {
    // Off is a supported choice: it is how you hear what an unbounded equalizer
    // actually does. The count is what turns "it sounds wrong" into "it clips".
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    limiter.setEnabled(false);

    constexpr int kCount = 4800;
    const std::vector<float> in = sine(1000.0f, 2.0f, kCount);
    std::vector<float> out(static_cast<std::size_t>(kCount), 0.0f);
    limiter.process(in.data(), out.data(), kCount);

    // Bit-identical, and no latency: disabled means out of the path.
    QCOMPARE(out, in);
    QVERIFY2(limiter.clipped() > 0, "clipping went unreported");
    QCOMPARE(limiter.latencySamples(), 0);
}

void LimiterTest::enabledNeverReportsClipping() {
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    limiter.setCeilingDb(-1.0f);
    limiter.setEnabled(true);

    constexpr int kCount = 4800;
    const std::vector<float> in = sine(1000.0f, 8.0f, kCount);
    std::vector<float> out(static_cast<std::size_t>(kCount), 0.0f);
    limiter.process(in.data(), out.data(), kCount);
    QCOMPARE(limiter.clipped(), 0);
}

void LimiterTest::lookaheadCostsLatencyOnlyWhileEnabled() {
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    QVERIFY(limiter.lookaheadSamples() > 0);
    limiter.setEnabled(true);
    QCOMPARE(limiter.latencySamples(), limiter.lookaheadSamples());
    limiter.setEnabled(false);
    QCOMPARE(limiter.latencySamples(), 0);
}

void LimiterTest::silenceStaysSilent() {
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate);
    limiter.setEnabled(true);
    std::vector<float> in(8192, 0.0f);
    std::vector<float> out(8192, 1.0f);
    limiter.process(in.data(), out.data(), 8192);
    for (const float v : out) {
        QCOMPARE(v, 0.0f);
    }
}

QTEST_APPLESS_MAIN(LimiterTest)
#include "LimiterTest.moc"
