#include <QtTest>

#include "audio/Mixer.h"

using sonar::audio::ChannelId;
using sonar::audio::ChannelLevel;
using sonar::audio::Mixer;

class MixerTest : public QObject {
    Q_OBJECT

private slots:
    void exposesAllChannels();
    void volumeIsClamped();
    void balanceIsClamped();
    void muteMakesChannelInaudible();
    void soloIsolatesChannels();
    void levelRoundTripsAndClamps();
};

void MixerTest::exposesAllChannels() {
    Mixer mixer;
    QCOMPARE(mixer.channels().size(), sonar::audio::kAllChannels.size());
}

void MixerTest::volumeIsClamped() {
    Mixer mixer;
    mixer.setVolume(ChannelId::Game, 2.0f);
    QCOMPARE(mixer.state(ChannelId::Game).volume, 1.0f);

    mixer.setVolume(ChannelId::Game, -0.5f);
    QCOMPARE(mixer.state(ChannelId::Game).volume, 0.0f);
}

void MixerTest::balanceIsClamped() {
    Mixer mixer;
    mixer.setBalance(ChannelId::Chat, 3.0f);
    QCOMPARE(mixer.state(ChannelId::Chat).balance, 1.0f);

    mixer.setBalance(ChannelId::Chat, -3.0f);
    QCOMPARE(mixer.state(ChannelId::Chat).balance, -1.0f);
}

void MixerTest::muteMakesChannelInaudible() {
    Mixer mixer;
    QVERIFY(mixer.isAudible(ChannelId::Media));
    mixer.setMuted(ChannelId::Media, true);
    QVERIFY(!mixer.isAudible(ChannelId::Media));
}

void MixerTest::soloIsolatesChannels() {
    Mixer mixer;
    mixer.setSolo(ChannelId::Game, true);

    QVERIFY(mixer.isAudible(ChannelId::Game));
    QVERIFY(!mixer.isAudible(ChannelId::Chat));
    QVERIFY(!mixer.isAudible(ChannelId::Media));

    // A muted solo channel is still silent.
    mixer.setMuted(ChannelId::Game, true);
    QVERIFY(!mixer.isAudible(ChannelId::Game));
}

void MixerTest::levelRoundTripsAndClamps() {
    Mixer mixer;
    mixer.setLevel(ChannelId::Aux, ChannelLevel{1.5f, -0.2f});
    const ChannelLevel lvl = mixer.level(ChannelId::Aux);
    QCOMPARE(lvl.peakLeft, 1.0f);
    QCOMPARE(lvl.peakRight, 0.0f);
}

QTEST_GUILESS_MAIN(MixerTest)
#include "MixerTest.moc"
