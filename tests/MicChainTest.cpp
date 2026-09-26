#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/MicChain.h"

namespace dsp = sonar::dsp;

// The microphone chain, checked against signals whose right answer is known
// before the code runs: a tone below the high-pass corner, a level under the
// gate's threshold, a level above the compressor's, and a signal far too loud
// for the limiter to let through.
class MicChainTest : public QObject {
    Q_OBJECT

private slots:
    void rawPresetChangesNothing();
    void muteProducesSilence();
    void highPassRemovesRumbleAndKeepsTheVoice();
    void gateClosesBelowItsThreshold();
    void gateOpensForSpeech();
    void gateHoldsThroughAShortGap();
    void compressorFollowsItsRatio();
    void autoMakeupRestoresTheLevel();
    void limiterNeverExceedsTheCeiling();
    void latencyIsTheLookaheadAndOnlyWhenLimiting();
    void survivesANonFiniteSample();
    void equalizerDeliversTheGainItIsAskedFor();
    void aFlatEqualizerChangesNothing();
    void equalizerBandsMeetInsteadOfLeavingHoles();
    void presetsAreSane();

private:
    static constexpr float kSampleRate = 48000.0f;

    // A sine, because every stage here is level-driven and a sine has exactly
    // one level.
    static std::vector<float> tone(float freqHz, float amplitude, int samples);
    static float peakDb(const std::vector<float>& buffer, int from = 0);
    static dsp::MicSettings onlyStage(void (*enable)(dsp::MicSettings&));
};

std::vector<float> MicChainTest::tone(float freqHz, float amplitude, int samples) {
    std::vector<float> out(static_cast<std::size_t>(samples));
    const float w = 2.0f * std::numbers::pi_v<float> * freqHz / kSampleRate;
    for (int i = 0; i < samples; ++i) {
        out[static_cast<std::size_t>(i)] = amplitude * std::sin(w * static_cast<float>(i));
    }
    return out;
}

float MicChainTest::peakDb(const std::vector<float>& buffer, int from) {
    float peak = 0.0f;
    for (std::size_t i = static_cast<std::size_t>(from); i < buffer.size(); ++i) {
        peak = std::max(peak, std::abs(buffer[i]));
    }
    return 20.0f * std::log10(std::max(peak, 1e-9f));
}

// Everything off, then the caller switches on the one stage under test — so a
// failure names a stage instead of a chain.
dsp::MicSettings MicChainTest::onlyStage(void (*enable)(dsp::MicSettings&)) {
    dsp::MicSettings s = dsp::presetSettings(dsp::MicPreset::Raw);
    enable(s);
    return s;
}

void MicChainTest::rawPresetChangesNothing() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(dsp::presetSettings(dsp::MicPreset::Raw));

    const std::vector<float> input = tone(1000.0f, 0.5f, 4800);
    std::vector<float> buffer = input;
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    // Bit-exact: with every stage off there is nothing left to be approximately
    // right about, and any drift here means a stage is running when it is off.
    for (std::size_t i = 0; i < input.size(); ++i) {
        QCOMPARE(buffer[i], input[i]);
    }
}

void MicChainTest::muteProducesSilence() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    dsp::MicSettings s = dsp::presetSettings(dsp::MicPreset::Podcast);
    s.muted = true;
    chain.setSettings(s);

    std::vector<float> buffer = tone(1000.0f, 0.9f, 2400);
    chain.process(buffer.data(), static_cast<int>(buffer.size()));
    for (const float sample : buffer) {
        QCOMPARE(sample, 0.0f);
    }
}

void MicChainTest::highPassRemovesRumbleAndKeepsTheVoice() {
    const auto settings = onlyStage([](dsp::MicSettings& s) {
        s.highPass.enabled = true;
        s.highPass.frequencyHz = 80.0f;
    });

    // Half an octave below the corner: a 12 dB/octave slope should be well into
    // its stop band. Desk bumps and footsteps live here.
    dsp::MicChain low;
    low.prepare(kSampleRate);
    low.setSettings(settings);
    std::vector<float> rumble = tone(40.0f, 0.5f, 24000);
    low.process(rumble.data(), static_cast<int>(rumble.size()));

    dsp::MicChain high;
    high.prepare(kSampleRate);
    high.setSettings(settings);
    std::vector<float> voice = tone(1000.0f, 0.5f, 24000);
    high.process(voice.data(), static_cast<int>(voice.size()));

    // Measured over the second half only: the first samples are the filter
    // settling, not its response.
    const float rumbleDb = peakDb(rumble, 12000);
    const float voiceDb = peakDb(voice, 12000);
    QVERIFY2(rumbleDb < -12.0f, qPrintable(QString("40 Hz came through at %1 dB").arg(rumbleDb)));
    QVERIFY2(voiceDb > -6.5f, qPrintable(QString("1 kHz was cut to %1 dB").arg(voiceDb)));
}

void MicChainTest::gateClosesBelowItsThreshold() {
    const auto settings = onlyStage([](dsp::MicSettings& s) { s.gate.enabled = true; });
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(settings);

    // -60 dBFS: a quiet room with the speaker not speaking. Threshold is -45 and
    // the ratio 4, so the expander should pull it about 45 dB further down.
    std::vector<float> buffer = tone(1000.0f, 0.001f, 48000);
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    // The last 100 ms only. The gate's release is 200 ms, so a window that
    // starts earlier measures it on the way down and calls that a failure.
    const float outDb = peakDb(buffer, 43200);
    QVERIFY2(outDb < -85.0f, qPrintable(QString("room noise left at %1 dB").arg(outDb)));
    QVERIFY(!chain.meters().gateOpen);
}

void MicChainTest::gateOpensForSpeech() {
    const auto settings = onlyStage([](dsp::MicSettings& s) { s.gate.enabled = true; });
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(settings);

    std::vector<float> buffer = tone(1000.0f, 0.1f, 48000);  // -20 dBFS
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(outDb > -21.0f, qPrintable(QString("speech was attenuated to %1 dB").arg(outDb)));
    QVERIFY(chain.meters().gateOpen);
}

void MicChainTest::gateHoldsThroughAShortGap() {
    const auto settings = onlyStage([](dsp::MicSettings& s) {
        s.gate.enabled = true;
        s.gate.holdMs = 120.0f;
    });
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(settings);

    std::vector<float> speech = tone(1000.0f, 0.1f, 24000);
    chain.process(speech.data(), static_cast<int>(speech.size()));
    QVERIFY(chain.meters().gateOpen);

    // 50 ms of nothing — the pause between two words, not the end of a sentence.
    // A gate that closes here chops the speaker up.
    std::vector<float> gap(2400, 0.0f);
    chain.process(gap.data(), static_cast<int>(gap.size()));
    QVERIFY2(chain.meters().gateOpen, "the gate closed inside a 50 ms gap");
}

void MicChainTest::compressorFollowsItsRatio() {
    auto settings = onlyStage([](dsp::MicSettings& s) { s.compressor.enabled = true; });
    settings.compressor.autoMakeup = false;  // measure the ratio, not the makeup
    settings.compressor.thresholdDb = -18.0f;
    settings.compressor.ratio = 3.0f;
    settings.compressor.kneeDb = 6.0f;

    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(settings);

    // -6 dBFS in, 12 dB above the threshold and clear of the knee: 3:1 leaves
    // 4 dB above it, so -14 dBFS out.
    std::vector<float> buffer = tone(1000.0f, 0.5f, 48000);
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(std::abs(outDb - (-14.0f)) < 1.0f,
             qPrintable(QString("expected about -14 dB, measured %1 dB").arg(outDb)));
    QVERIFY(chain.meters().compressorReductionDb < -6.0f);
}

void MicChainTest::autoMakeupRestoresTheLevel() {
    auto settings = onlyStage([](dsp::MicSettings& s) { s.compressor.enabled = true; });
    settings.compressor.autoMakeup = true;

    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(settings);

    // A voice sitting at the reference the makeup is calculated for comes out
    // where it went in: compression that quietly turns everything down is a
    // compressor the user will fight with the volume slider.
    std::vector<float> buffer = tone(1000.0f, 0.3162f, 48000);  // -10 dBFS
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(std::abs(outDb - (-10.0f)) < 1.0f,
             qPrintable(QString("-10 dB in became %1 dB out").arg(outDb)));
}

void MicChainTest::limiterNeverExceedsTheCeiling() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    dsp::MicSettings s = dsp::presetSettings(dsp::MicPreset::Streaming);
    s.inputGainDb = 24.0f;  // someone who set their gain wrong, which is everyone
    s.limiter.ceilingDb = -1.0f;
    chain.setSettings(s);

    std::vector<float> buffer = tone(220.0f, 1.0f, 48000);
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    for (const float sample : buffer) {
        QVERIFY2(std::abs(sample) <= ceiling + 1e-6f,
                 qPrintable(QString("sample %1 is past the ceiling").arg(sample)));
    }
}

void MicChainTest::latencyIsTheLookaheadAndOnlyWhenLimiting() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);

    dsp::MicSettings s = dsp::presetSettings(dsp::MicPreset::Podcast);
    s.limiter.enabled = true;
    chain.setSettings(s);
    // 2 ms at 48 kHz. The monitoring path has to know, or the user hears
    // themselves late by an amount nobody can explain.
    QCOMPARE(chain.latencySamples(), 96);

    s.limiter.enabled = false;
    chain.setSettings(s);
    QCOMPARE(chain.latencySamples(), 0);
}

void MicChainTest::survivesANonFiniteSample() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(dsp::presetSettings(dsp::MicPreset::Podcast));

    // A device that disappears mid-stream can hand over a buffer of garbage. One
    // NaN through a feedback path poisons every filter state for good, and the
    // microphone stays silent until the app is restarted.
    std::vector<float> buffer = tone(1000.0f, 0.4f, 4800);
    buffer[100] = std::numeric_limits<float>::quiet_NaN();
    buffer[101] = std::numeric_limits<float>::infinity();
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    for (const float sample : buffer) {
        QVERIFY(std::isfinite(sample));
    }

    std::vector<float> after = tone(1000.0f, 0.4f, 4800);
    chain.process(after.data(), static_cast<int>(after.size()));
    QVERIFY2(peakDb(after, 2400) > -30.0f, "the chain stayed silent after a NaN");
}

void MicChainTest::equalizerDeliversTheGainItIsAskedFor() {
    // Everything else off: this measures the equalizer, not the compressor's
    // opinion of it.
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(dsp::presetSettings(dsp::MicPreset::Raw));

    dsp::EqSettings eq;
    dsp::resetBands(eq, dsp::BandCount::Bands10);
    eq.enabled = true;
    const std::vector<float> freqs = dsp::standardFrequencies(dsp::BandCount::Bands10);

    // The band nearest 1 kHz — where a voice lives and where a boost should be
    // impossible to miss.
    std::size_t band = 0;
    for (std::size_t i = 0; i < freqs.size(); ++i) {
        if (std::abs(freqs[i] - 1000.0f) < std::abs(freqs[band] - 1000.0f)) {
            band = i;
        }
    }
    eq.bands[band].gainDb = 10.0f;
    chain.setEqualizer(eq);

    std::vector<float> buffer = tone(freqs[band], 0.1f, 48000);  // -20 dBFS in
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    // +10 dB asked for, so -10 dBFS out. A band that arrives as +2 dB is a band
    // the user will drag to the top and still not hear.
    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(std::abs(outDb - (-10.0f)) < 1.5f,
             qPrintable(QString("+10 dB at %1 Hz produced %2 dBFS, expected about -10")
                            .arg(freqs[band]).arg(outDb)));
}

void MicChainTest::aFlatEqualizerChangesNothing() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(dsp::presetSettings(dsp::MicPreset::Raw));

    dsp::EqSettings eq;
    dsp::resetBands(eq, dsp::BandCount::Bands10);
    eq.enabled = true;  // on, but every band at zero
    chain.setEqualizer(eq);

    std::vector<float> buffer = tone(1000.0f, 0.1f, 48000);
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    // Switching the equalizer on must not make the microphone quieter. It used
    // to: the chain attenuated by whatever the curve boosted, which on a flat
    // curve is nothing — but the same compensation is what cancelled every boost.
    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(std::abs(outDb - (-20.0f)) < 0.7f,
             qPrintable(QString("a flat equalizer moved the level to %1 dBFS").arg(outDb)));
}

void MicChainTest::equalizerBandsMeetInsteadOfLeavingHoles() {
    dsp::MicChain chain;
    chain.prepare(kSampleRate);
    chain.setSettings(dsp::presetSettings(dsp::MicPreset::Raw));

    dsp::EqSettings eq;
    dsp::resetBands(eq, dsp::BandCount::Bands10);
    eq.enabled = true;
    for (dsp::EqBand& band : eq.bands) {
        band.gainDb = 6.0f;  // lift the whole curve
    }
    chain.setEqualizer(eq);

    // Halfway between two band centres, geometrically. With a Q meant for
    // third-octave spacing the filters here are narrow spikes and this frequency
    // falls into the gap between two of them, so a curve the user raised by 6 dB
    // arrives as barely anything.
    const std::vector<float> freqs = dsp::standardFrequencies(dsp::BandCount::Bands10);
    const float between = std::sqrt(freqs[3] * freqs[4]);

    std::vector<float> buffer = tone(between, 0.1f, 48000);  // -20 dBFS in
    chain.process(buffer.data(), static_cast<int>(buffer.size()));

    const float outDb = peakDb(buffer, 24000);
    QVERIFY2(outDb > -16.0f,
             qPrintable(QString("a +6 dB curve gave %1 dBFS at %2 Hz, between two bands")
                            .arg(outDb).arg(between)));
}

void MicChainTest::presetsAreSane() {
    for (const dsp::MicPreset preset :
         {dsp::MicPreset::Raw, dsp::MicPreset::Podcast, dsp::MicPreset::Streaming,
          dsp::MicPreset::Meeting}) {
        const dsp::MicSettings s = dsp::presetSettings(preset);
        QVERIFY(!dsp::micPresetName(preset).empty());
        QVERIFY(s.gate.ratio >= 1.0f);
        QVERIFY(s.compressor.ratio >= 1.0f);
        QVERIFY(s.deEsser.ratio >= 1.0f);
        // A ceiling above full scale is not a ceiling.
        QVERIFY(s.limiter.ceilingDb <= 0.0f);
        // The gate must not be able to cut into speech: a threshold above the
        // compressor's is a preset that gates the voice it then compresses.
        QVERIFY(s.gate.thresholdDb < s.compressor.thresholdDb);
    }
}

QTEST_GUILESS_MAIN(MicChainTest)
#include "MicChainTest.moc"
