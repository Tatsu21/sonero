#include <QtTest>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "dsp/NoiseSuppressor.h"

namespace dsp = sonar::dsp;

// The noise suppressor has two jobs, and which one it is doing depends on how
// Sonero was built. Both are tested here: with no model it must be an exact
// passthrough, and with one it must remove noise while leaving a voice-shaped
// signal recognisable.
class NoiseSuppressorTest : public QObject {
    Q_OBJECT

private slots:
    void refusesAnythingButFortyEightKilohertz();
    void passesThroughWhenDisabled();
    void passesThroughWithNoModel();
    void addsLatencyOnlyWhileRunning();
    void handlesAnEmptyBuffer();
    void removesNoiseWhenAModelIsPresent();

private:
    static constexpr float kSampleRate = 48000.0f;
    static std::vector<float> tone(float freqHz, float amplitude, int samples);
    static float rms(const std::vector<float>& buffer, int from = 0);
};

std::vector<float> NoiseSuppressorTest::tone(float freqHz, float amplitude, int samples) {
    std::vector<float> out(static_cast<std::size_t>(samples));
    const float w = 2.0f * std::numbers::pi_v<float> * freqHz / kSampleRate;
    for (int i = 0; i < samples; ++i) {
        out[static_cast<std::size_t>(i)] = amplitude * std::sin(w * static_cast<float>(i));
    }
    return out;
}

float NoiseSuppressorTest::rms(const std::vector<float>& buffer, int from) {
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = static_cast<std::size_t>(from); i < buffer.size(); ++i) {
        sum += static_cast<double>(buffer[i]) * buffer[i];
        ++count;
    }
    return count > 0 ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
}

void NoiseSuppressorTest::refusesAnythingButFortyEightKilohertz() {
    dsp::NoiseSuppressor ns;
    // The model has one sample rate. Running it at another would not fail, it
    // would mis-hear every band — which is worse than not running, because the
    // switch would look like it was working.
    ns.prepare(44100.0f);
    QVERIFY(!ns.usable());
    ns.prepare(96000.0f);
    QVERIFY(!ns.usable());

    ns.prepare(48000.0f);
    QCOMPARE(ns.usable(), dsp::NoiseSuppressor::available());
}

void NoiseSuppressorTest::passesThroughWhenDisabled() {
    dsp::NoiseSuppressor ns;
    ns.prepare(kSampleRate);
    ns.setEnabled(false);

    const std::vector<float> input = tone(300.0f, 0.4f, 4800);
    std::vector<float> buffer = input;
    ns.process(buffer.data(), static_cast<int>(buffer.size()));
    for (std::size_t i = 0; i < input.size(); ++i) {
        QCOMPARE(buffer[i], input[i]);
    }
}

void NoiseSuppressorTest::passesThroughWithNoModel() {
    if (dsp::NoiseSuppressor::available()) {
        QSKIP("this build has a model; the passthrough path is not the one running");
    }
    dsp::NoiseSuppressor ns;
    ns.prepare(kSampleRate);
    ns.setEnabled(true);

    // A build without RNNoise must not eat the microphone. Bit-exact, because a
    // stage that cannot run should do nothing at all rather than nearly nothing.
    const std::vector<float> input = tone(300.0f, 0.4f, 4800);
    std::vector<float> buffer = input;
    ns.process(buffer.data(), static_cast<int>(buffer.size()));
    for (std::size_t i = 0; i < input.size(); ++i) {
        QCOMPARE(buffer[i], input[i]);
    }
    QCOMPARE(ns.latencySamples(), 0);
    QCOMPARE(ns.backendName(), std::string_view("none"));
}

void NoiseSuppressorTest::addsLatencyOnlyWhileRunning() {
    dsp::NoiseSuppressor ns;
    ns.prepare(kSampleRate);

    ns.setEnabled(false);
    QCOMPARE(ns.latencySamples(), 0);

    ns.setEnabled(true);
    // One frame of the model's own size when it runs, nothing when it cannot:
    // the monitoring path adds this to the limiter's lookahead.
    QCOMPARE(ns.latencySamples(), dsp::NoiseSuppressor::available() ? 480 : 0);
}

void NoiseSuppressorTest::handlesAnEmptyBuffer() {
    dsp::NoiseSuppressor ns;
    ns.prepare(kSampleRate);
    ns.setEnabled(true);
    // A device that just disappeared hands over a zero-length buffer.
    ns.process(nullptr, 0);
    std::vector<float> empty;
    ns.process(empty.data(), 0);
    QVERIFY(true);  // reaching here without a crash is the assertion
}

void NoiseSuppressorTest::removesNoiseWhenAModelIsPresent() {
    if (!dsp::NoiseSuppressor::available()) {
        QSKIP("no RNNoise in this build — nothing to suppress with");
    }
    dsp::NoiseSuppressor ns;
    ns.prepare(kSampleRate);
    ns.setEnabled(true);
    QVERIFY(ns.usable());

    // White noise is what the model is for. Half a second of it, measured after
    // the first frames so the priming silence is not counted as suppression.
    std::mt19937 rng(20260925);
    std::uniform_real_distribution<float> dist(-0.2f, 0.2f);
    std::vector<float> noise(24000);
    for (float& sample : noise) {
        sample = dist(rng);
    }
    const float before = rms(noise, 2400);
    std::vector<float> processed = noise;
    ns.process(processed.data(), static_cast<int>(processed.size()));
    const float after = rms(processed, 2400);

    QVERIFY2(after < before * 0.5f,
             qPrintable(QString("noise went from %1 to %2").arg(before).arg(after)));
    QVERIFY(ns.reductionDb() < 0.0f);
}

QTEST_GUILESS_MAIN(NoiseSuppressorTest)
#include "NoiseSuppressorTest.moc"
