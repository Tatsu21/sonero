#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/HarmonicBass.h"

namespace dsp = sonar::dsp;

namespace {

constexpr float kSampleRate = 48000.0f;

// Amplitude of a single frequency in a buffer, by direct correlation. A full FFT
// would tell us about frequencies nobody is asking about; what every test here
// needs is "how much 150 Hz is in this signal", which is two sums.
double amplitudeAt(const std::vector<float>& buf, float freqHz, int from, int to) {
    double re = 0.0;
    double im = 0.0;
    const double w = 2.0 * std::numbers::pi * freqHz / kSampleRate;
    for (int n = from; n < to; ++n) {
        const double x = buf[static_cast<std::size_t>(n)];
        re += x * std::cos(w * n);
        im -= x * std::sin(w * n);
    }
    const double count = to - from;
    return 2.0 * std::sqrt(re * re + im * im) / count;
}

double meanOf(const std::vector<float>& buf, int from, int to) {
    double sum = 0.0;
    for (int n = from; n < to; ++n) {
        sum += buf[static_cast<std::size_t>(n)];
    }
    return sum / (to - from);
}

std::vector<float> sine(float freqHz, float amplitude, int count) {
    std::vector<float> buf(static_cast<std::size_t>(count), 0.0f);
    const double w = 2.0 * std::numbers::pi * freqHz / kSampleRate;
    for (int n = 0; n < count; ++n) {
        buf[static_cast<std::size_t>(n)] = static_cast<float>(amplitude * std::sin(w * n));
    }
    return buf;
}

dsp::HarmonicBassSettings defaults() {
    dsp::HarmonicBassSettings s;
    s.enabled = true;
    s.crossoverHz = 90.0f;
    s.amountDb = 0.0f;  // unity, so the tests measure the algorithm not a trim
    s.harmonics = 4;
    s.evenWeight = 0.5f;
    s.replaceLows = false;
    return s;
}

}  // namespace

class HarmonicBassTest : public QObject {
    Q_OBJECT

private slots:
    void disabledLeavesTheSignalUntouched();
    void generatesHarmonicsThatWereNotInTheInput();
    void writesToASeparateOutputBuffer();
    void harmonicCountBoundsWhatIsGenerated();
    void harmonicsTrackTheInputLevel();
    void addsNoDcOffset();
    void silenceInSilenceOut();
    void replaceLowsRemovesTheFundamental();
    void leavesMidrangeAlone();
};

void HarmonicBassTest::disabledLeavesTheSignalUntouched() {
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    dsp::HarmonicBassSettings s = defaults();
    s.enabled = false;
    bass.setSettings(s);

    std::vector<float> buf = sine(50.0f, 0.5f, 4096);
    const std::vector<float> before = buf;
    bass.process(buf.data(), 4096);
    // Bit-identical, not merely close: a disabled stage must not be in the path
    // at all. Anything else means the filters are still running and the channel
    // is paying for a feature that is switched off.
    QCOMPARE(buf, before);
}

void HarmonicBassTest::generatesHarmonicsThatWereNotInTheInput() {
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    bass.setSettings(defaults());

    // 50 Hz, well below the 90 Hz crossover: the frequency a small driver cannot
    // reproduce, and the whole reason this stage exists.
    constexpr int kFrames = 48000;
    std::vector<float> buf = sine(50.0f, 0.4f, kFrames);
    const std::vector<float> before = buf;
    bass.process(buf.data(), kFrames);

    // Measure after the filters and the envelope have settled.
    constexpr int kFrom = 24000;

    // The input is a pure sine: it has no harmonics at all. This is the claim the
    // whole algorithm rests on, so it is checked against the input rather than
    // against a threshold picked to pass.
    for (const float h : {100.0f, 150.0f, 200.0f}) {
        const double in = amplitudeAt(before, h, kFrom, kFrames);
        const double out = amplitudeAt(buf, h, kFrom, kFrames);
        QVERIFY2(in < 1e-4, qPrintable(QStringLiteral("input already had %1 Hz: %2")
                                           .arg(h)
                                           .arg(in)));
        QVERIFY2(out > 0.01, qPrintable(QStringLiteral("no %1 Hz generated: %2")
                                            .arg(h)
                                            .arg(out)));
    }
}

void HarmonicBassTest::writesToASeparateOutputBuffer() {
    // A LADSPA host is entitled to hand a plugin distinct input and output
    // buffers. Processing out-of-place must give the same answer as in-place,
    // and must not depend on what the output buffer held beforehand.
    const auto run = [](bool inPlace) {
        dsp::HarmonicBass bass;
        bass.prepare(kSampleRate);
        bass.setSettings(defaults());
        constexpr int kCount = 24000;
        std::vector<float> in = sine(50.0f, 0.4f, kCount);
        if (inPlace) {
            bass.process(in.data(), kCount);
            return in;
        }
        // Deliberately not zeroed: if the stage ever accumulated into its output
        // instead of writing it, this is what would catch it.
        std::vector<float> out(static_cast<std::size_t>(kCount), 7.0f);
        bass.process(in.data(), out.data(), kCount);
        return out;
    };
    QCOMPARE(run(false), run(true));
}

void HarmonicBassTest::harmonicCountBoundsWhatIsGenerated() {
    // Two orders means the 2nd harmonic and nothing above it. The band limit is
    // what keeps a low setting from leaking the orders it was told not to make.
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    dsp::HarmonicBassSettings s = defaults();
    s.harmonics = 2;
    bass.setSettings(s);

    constexpr int kFrames = 48000;
    std::vector<float> buf = sine(50.0f, 0.4f, kFrames);
    bass.process(buf.data(), kFrames);
    constexpr int kFrom = 24000;

    const double second = amplitudeAt(buf, 100.0f, kFrom, kFrames);
    const double fifth = amplitudeAt(buf, 250.0f, kFrom, kFrames);
    QVERIFY2(second > 0.01, qPrintable(QStringLiteral("2nd missing: %1").arg(second)));
    // The 5th was never requested, and the lowpass sits well below it.
    QVERIFY2(fifth < second * 0.1, qPrintable(QStringLiteral("5th leaked: %1 vs %2")
                                                  .arg(fifth)
                                                  .arg(second)));
}

void HarmonicBassTest::harmonicsTrackTheInputLevel() {
    // The one property that separates this from a distortion pedal: the
    // harmonics are proportional to the bass that produced them. Halve the input
    // and the harmonics halve too. A waveshaper without the envelope
    // normalisation would instead change its harmonic content with level, which
    // is what makes an enhancer sound like an effect.
    const auto harmonicAt = [](float amplitude) {
        dsp::HarmonicBass bass;
        bass.prepare(kSampleRate);
        bass.setSettings(defaults());
        constexpr int kFrames = 48000;
        std::vector<float> buf = sine(50.0f, amplitude, kFrames);
        bass.process(buf.data(), kFrames);
        return amplitudeAt(buf, 150.0f, 24000, kFrames);
    };

    const double loud = harmonicAt(0.4f);
    const double quiet = harmonicAt(0.2f);
    QVERIFY(loud > 0.01);
    // Within 1 dB of a factor of two.
    const double ratio = loud / quiet;
    QVERIFY2(ratio > 1.78 && ratio < 2.25,
             qPrintable(QStringLiteral("harmonics do not track level: ratio %1").arg(ratio)));
}

void HarmonicBassTest::addsNoDcOffset() {
    // The even-order Chebyshev polynomials each carry a constant term — T2(u) is
    // 2u^2 - 1 — so the shaper's raw output has a DC component that moves with
    // the envelope. Left in, it would eat headroom and thump on every note.
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    dsp::HarmonicBassSettings s = defaults();
    s.harmonics = 2;      // even only: the worst case for DC
    s.evenWeight = 1.0f;
    bass.setSettings(s);

    constexpr int kFrames = 48000;
    std::vector<float> buf = sine(50.0f, 0.5f, kFrames);
    bass.process(buf.data(), kFrames);

    const double dc = meanOf(buf, 24000, kFrames);
    QVERIFY2(std::abs(dc) < 1e-3, qPrintable(QStringLiteral("DC offset %1").arg(dc)));
}

void HarmonicBassTest::silenceInSilenceOut() {
    // The shaper divides by the envelope. Fed silence it must stay silent rather
    // than amplifying the noise floor into a bed of harmonics, which is what an
    // unguarded normalisation would do.
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    bass.setSettings(defaults());

    std::vector<float> buf(8192 * 2, 0.0f);
    bass.process(buf.data(), 8192);
    for (const float v : buf) {
        QVERIFY(std::abs(v) < 1e-9f);
    }

    // And a signal far below the envelope floor is not inflated either.
    std::vector<float> tiny = sine(50.0f, 1e-6f, 8192);
    bass.process(tiny.data(), 8192);
    for (const float v : tiny) {
        QVERIFY(std::abs(v) < 1e-4f);
    }
}

void HarmonicBassTest::replaceLowsRemovesTheFundamental() {
    // "Replace" rather than "add": the driver stops being asked for what it
    // cannot produce, and the headroom goes to the harmonics instead.
    constexpr int kFrames = 48000;
    constexpr int kFrom = 24000;

    const auto fundamentalWith = [](bool replace) {
        dsp::HarmonicBass bass;
        bass.prepare(kSampleRate);
        dsp::HarmonicBassSettings s = defaults();
        s.replaceLows = replace;
        bass.setSettings(s);
        std::vector<float> buf = sine(40.0f, 0.4f, kFrames);
        bass.process(buf.data(), kFrames);
        return amplitudeAt(buf, 40.0f, kFrom, kFrames);
    };

    const double kept = fundamentalWith(false);
    const double removed = fundamentalWith(true);

    // Left in, the fundamental is essentially untouched: this stage adds, it does
    // not filter.
    QVERIFY2(kept > 0.35, qPrintable(QStringLiteral("fundamental lost: %1").arg(kept)));
    // Taken out, 40 Hz against a 90 Hz Linkwitz-Riley 4th-order corner is more
    // than an octave down the slope — at least 20 dB.
    QVERIFY2(removed < kept * 0.1,
             qPrintable(QStringLiteral("fundamental not removed: %1 vs %2")
                            .arg(removed)
                            .arg(kept)));
}

void HarmonicBassTest::leavesMidrangeAlone() {
    // Nothing above the crossover is the bass stage's business. A voice must come
    // out as it went in, or the stage is a distortion unit wearing a bass badge.
    dsp::HarmonicBass bass;
    bass.prepare(kSampleRate);
    bass.setSettings(defaults());

    constexpr int kFrames = 48000;
    constexpr int kFrom = 24000;
    std::vector<float> buf = sine(1000.0f, 0.3f, kFrames);
    bass.process(buf.data(), kFrames);

    const double kept = amplitudeAt(buf, 1000.0f, kFrom, kFrames);
    QVERIFY2(std::abs(kept - 0.3) < 0.01,
             qPrintable(QStringLiteral("1 kHz changed: %1").arg(kept)));
    // And it generated no second harmonic of its own.
    const double second = amplitudeAt(buf, 2000.0f, kFrom, kFrames);
    QVERIFY2(second < 0.003, qPrintable(QStringLiteral("2 kHz appeared: %1").arg(second)));
}

QTEST_APPLESS_MAIN(HarmonicBassTest)
#include "HarmonicBassTest.moc"
