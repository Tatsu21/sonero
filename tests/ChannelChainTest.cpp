#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/ChannelChain.h"

namespace dsp = sonar::dsp;

namespace {

constexpr float kSampleRate = 48000.0f;

double amplitudeAt(const std::vector<float>& buf, float freqHz, int channel, int from,
                   int to) {
    double re = 0.0;
    double im = 0.0;
    const double w = 2.0 * std::numbers::pi * freqHz / kSampleRate;
    for (int n = from; n < to; ++n) {
        const double x = buf[static_cast<std::size_t>(2 * n + channel)];
        re += x * std::cos(w * n);
        im -= x * std::sin(w * n);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (to - from);
}

std::vector<float> sine(float freqHz, float amplitude, int frames) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0f);
    const double w = 2.0 * std::numbers::pi * freqHz / kSampleRate;
    for (int n = 0; n < frames; ++n) {
        const auto v = static_cast<float>(amplitude * std::sin(w * n));
        buf[static_cast<std::size_t>(2 * n)] = v;
        buf[static_cast<std::size_t>(2 * n + 1)] = v;
    }
    return buf;
}

dsp::EqSettings oneBand(float freqHz, float gainDb) {
    dsp::EqSettings s;
    s.enabled = true;
    dsp::resetBands(s, dsp::BandCount::Bands10);
    for (dsp::EqBand& band : s.bands) {
        if (std::abs(band.frequency - freqHz) < 0.5f) {
            band.gainDb = gainDb;
        }
    }
    return s;
}

}  // namespace

class ChannelChainTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsAreCloseToTransparent();
    void mutedIsSilent();
    void faderScalesTheOutput();
    void balancePansByAttenuatingNeverByBoosting();
    void equalizerBoostIsCompensatedForHeadroom();
    void equalizerGainIsClampedToTheSupportedRange();
    void bassStageRunsAfterTheEqualizer();
    void bassIsIndependentPerChannel();
    void limiterHoldsTheCeiling();
    void blockSizeDoesNotChangeTheResult();
};

void ChannelChainTest::defaultsAreCloseToTransparent() {
    // A fresh channel must pass audio essentially untouched, bar the half a
    // decibel of reserve the flat cascade keeps against full-scale material.
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    std::vector<float> buf = sine(1000.0f, 0.2f, 24000);
    chain.process(buf.data(), 24000);

    const double out = amplitudeAt(buf, 1000.0f, 0, 12000, 24000);
    const double db = 20.0 * std::log10(out / 0.2);
    QVERIFY2(db < 0.1 && db > -1.0,
             qPrintable(QStringLiteral("default chain changed the level by %1 dB").arg(db)));
}

void ChannelChainTest::mutedIsSilent() {
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    dsp::ChannelChainSettings s;
    s.muted = true;
    chain.setSettings(s);
    std::vector<float> buf = sine(1000.0f, 0.5f, 4096);
    chain.process(buf.data(), 4096);
    for (const float v : buf) {
        QCOMPARE(v, 0.0f);
    }
}

void ChannelChainTest::faderScalesTheOutput() {
    const auto levelAt = [](float volume) {
        dsp::ChannelChain chain;
        chain.prepare(kSampleRate);
        dsp::ChannelChainSettings s;
        s.volume = volume;
        chain.setSettings(s);
        std::vector<float> buf = sine(1000.0f, 0.2f, 24000);
        chain.process(buf.data(), 24000);
        return amplitudeAt(buf, 1000.0f, 0, 12000, 24000);
    };
    const double full = levelAt(1.0f);
    const double half = levelAt(0.5f);
    QVERIFY(full > 0.1);
    QVERIFY2(std::abs(half / full - 0.5) < 0.01,
             qPrintable(QStringLiteral("half volume gave %1 of full").arg(half / full)));
}

void ChannelChainTest::balancePansByAttenuatingNeverByBoosting() {
    // Panning by boosting the far side would add gain to a channel that may
    // already be at full scale, which is how a balance control becomes a
    // clipping control.
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    dsp::ChannelChainSettings s;
    s.balance = -1.0f;  // hard left
    chain.setSettings(s);

    std::vector<float> buf = sine(1000.0f, 0.2f, 24000);
    chain.process(buf.data(), 24000);
    const double left = amplitudeAt(buf, 1000.0f, 0, 12000, 24000);
    const double right = amplitudeAt(buf, 1000.0f, 1, 12000, 24000);

    QVERIFY2(right < left * 0.01, "hard left did not attenuate the right");
    // The kept side is not louder than it was.
    QVERIFY2(left <= 0.2 * 1.02,
             qPrintable(QStringLiteral("panning boosted the left to %1").arg(left)));
}

void ChannelChainTest::equalizerBoostIsCompensatedForHeadroom() {
    // The channel sinks have nothing downstream to protect them, so a boosting
    // curve is paid for by an equal attenuation ahead of the filters: the
    // boosted band lands back near unity instead of clipping on the way out.
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    chain.setEqualizer(oneBand(1000.0f, 10.0f));

    std::vector<float> buf = sine(1000.0f, 0.2f, 48000);
    chain.process(buf.data(), 48000);
    const double out = amplitudeAt(buf, 1000.0f, 0, 24000, 48000);
    const double db = 20.0 * std::log10(out / 0.2);
    QVERIFY2(std::abs(db) < 2.0,
             qPrintable(QStringLiteral("+10 dB band arrived at %1 dB — headroom "
                                       "compensation is wrong")
                            .arg(db)));
}

void ChannelChainTest::equalizerGainIsClampedToTheSupportedRange() {
    // An imported preset asking for +40 dB gets kMaxGainDb, not +40. These are
    // the shipped channels; the gain ceiling is what keeps them predictable.
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    chain.setEqualizer(oneBand(1000.0f, 40.0f));

    std::vector<float> buf = sine(1000.0f, 0.02f, 48000);
    chain.process(buf.data(), 48000);
    const double out = amplitudeAt(buf, 1000.0f, 0, 24000, 48000);
    // Clamped to +12 and then compensated, so it lands near unity — not at +40.
    QVERIFY2(out < 0.02 * 4.0,
             qPrintable(QStringLiteral("a +40 dB band was not clamped: %1").arg(out)));
}

void ChannelChainTest::bassStageRunsAfterTheEqualizer() {
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    dsp::BassStageSettings b;
    b.bass.enabled = true;
    b.bass.crossoverHz = 90.0f;
    b.bass.amountDb = 6.0f;
    b.bass.harmonics = 4;
    chain.setBass(b);

    constexpr int kFrames = 48000;
    std::vector<float> buf = sine(50.0f, 0.3f, kFrames);
    chain.process(buf.data(), kFrames);

    // A pure 50 Hz sine has no 150 Hz in it; anything there was synthesised.
    QVERIFY2(amplitudeAt(buf, 150.0f, 0, 24000, kFrames) > 0.005,
             "the bass stage produced nothing through the chain");
    QVERIFY(chain.meters().bass.active);
}

void ChannelChainTest::bassIsIndependentPerChannel() {
    // Each side runs its own instance, so a signal present only on the left must
    // not produce harmonics on the right. A single shared instance would leak.
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    dsp::BassStageSettings b;
    b.bass.enabled = true;
    b.bass.crossoverHz = 90.0f;
    chain.setBass(b);

    constexpr int kFrames = 48000;
    std::vector<float> buf(static_cast<std::size_t>(kFrames) * 2, 0.0f);
    for (int n = 0; n < kFrames; ++n) {
        buf[static_cast<std::size_t>(2 * n)] =
            static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 50.0 * n / kSampleRate));
    }
    chain.process(buf.data(), kFrames);

    QVERIFY(amplitudeAt(buf, 150.0f, 0, 24000, kFrames) > 0.005);
    QVERIFY2(amplitudeAt(buf, 150.0f, 1, 24000, kFrames) < 1e-4,
             "harmonics leaked into a silent channel");
}

void ChannelChainTest::limiterHoldsTheCeiling() {
    dsp::ChannelChain chain;
    chain.prepare(kSampleRate);
    dsp::BassStageSettings b;
    b.limiterEnabled = true;
    b.limiterCeilingDb = -1.0f;
    chain.setBass(b);
    dsp::ChannelChainSettings s;
    s.trimDb = 12.0f;  // drive it well past full scale
    chain.setSettings(s);

    std::vector<float> buf = sine(1000.0f, 0.9f, 24000);
    chain.process(buf.data(), 24000);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    for (const float v : buf) {
        QVERIFY2(std::abs(v) <= ceiling + 1e-5f,
                 qPrintable(QStringLiteral("sample past the ceiling: %1").arg(v)));
    }
}

void ChannelChainTest::blockSizeDoesNotChangeTheResult() {
    // PipeWire decides the block size, and it varies. A chain whose output
    // depends on it has state that is not carried across calls — which would
    // show up as a click on every quantum change.
    const auto run = [](int block) {
        dsp::ChannelChain chain;
        chain.prepare(kSampleRate);
        dsp::BassStageSettings b;
        b.bass.enabled = true;
        chain.setBass(b);
        chain.setEqualizer(oneBand(1000.0f, 6.0f));
        std::vector<float> buf = sine(50.0f, 0.3f, 12000);
        for (int done = 0; done < 12000; done += block) {
            chain.process(buf.data() + static_cast<std::size_t>(done) * 2,
                          std::min(block, 12000 - done));
        }
        return buf;
    };
    const std::vector<float> a = run(480);
    const std::vector<float> b = run(97);  // deliberately not a divisor
    QCOMPARE(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        QVERIFY2(std::abs(a[i] - b[i]) < 1e-5f,
                 qPrintable(QStringLiteral("block size changed sample %1: %2 vs %3")
                                .arg(i)
                                .arg(a[i])
                                .arg(b[i])));
    }
}

QTEST_APPLESS_MAIN(ChannelChainTest)
#include "ChannelChainTest.moc"
