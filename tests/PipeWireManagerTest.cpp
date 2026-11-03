#include <QtTest>

#include "audio/PipeWireManager.h"

using sonar::audio::BackendState;
using sonar::audio::PipeWireManager;

// These tests exercise the lifecycle state machine. They are written to be
// environment-tolerant: whether or not a PipeWire daemon is reachable,
// initialize() must resolve to a *definite* state and shutdown() must reset it.
class PipeWireManagerTest : public QObject {
    Q_OBJECT

private slots:
    void initialStateIsUninitialized();
    void backendNameIsPipeWire();
    void initializeYieldsDefiniteState();
    void shutdownResetsState();
    void doubleInitializeIsIdempotent();
    void virtualSinkNamesAreDerivedFromChannel();
    void micFollowsTheDefaultWhenNothingIsChosen();
    void micPrefersTheChosenDeviceWhileConnected();
    void micLeavesABluetoothDefaultAlone();
};

void PipeWireManagerTest::initialStateIsUninitialized() {
    PipeWireManager manager;
    QVERIFY(manager.state() == BackendState::Uninitialized);
    QVERIFY(!manager.isAvailable());
}

void PipeWireManagerTest::backendNameIsPipeWire() {
    PipeWireManager manager;
    QVERIFY(manager.name() == "PipeWire");
}

void PipeWireManagerTest::initializeYieldsDefiniteState() {
    PipeWireManager manager;
    manager.initialize();

    const BackendState state = manager.state();
    QVERIFY(state == BackendState::Available || state == BackendState::Unavailable);
    // isAvailable() must be consistent with state().
    QVERIFY(manager.isAvailable() == (state == BackendState::Available));

    manager.shutdown();
}

void PipeWireManagerTest::shutdownResetsState() {
    PipeWireManager manager;
    manager.initialize();
    manager.shutdown();
    QVERIFY(manager.state() == BackendState::Uninitialized);
    QVERIFY(!manager.isAvailable());
}

void PipeWireManagerTest::doubleInitializeIsIdempotent() {
    PipeWireManager manager;
    const bool first = manager.initialize();
    const BackendState afterFirst = manager.state();
    const bool second = manager.initialize();

    QVERIFY(first == second);
    QVERIFY(manager.state() == afterFirst);

    manager.shutdown();
}

void PipeWireManagerTest::virtualSinkNamesAreDerivedFromChannel() {
    using sonar::audio::ChannelId;
    QCOMPARE(QString::fromStdString(PipeWireManager::nodeNameFor(ChannelId::Game)),
             QStringLiteral("sonero_game"));
    QCOMPARE(QString::fromStdString(PipeWireManager::nodeNameFor(ChannelId::Microphone)),
             QStringLiteral("sonero_microphone"));
}

void PipeWireManagerTest::micFollowsTheDefaultWhenNothingIsChosen() {
    const std::vector<std::string> connected{"usb_mic", "builtin"};
    QCOMPARE(PipeWireManager::resolveMicTarget("", "builtin", connected), std::string("builtin"));
    // A stale default, announced before or after its device, is not a target.
    QCOMPARE(PipeWireManager::resolveMicTarget("", "gone", connected), std::string());
}

void PipeWireManagerTest::micPrefersTheChosenDeviceWhileConnected() {
    const std::vector<std::string> connected{"usb_mic", "builtin"};
    QCOMPARE(PipeWireManager::resolveMicTarget("usb_mic", "builtin", connected),
             std::string("usb_mic"));
    // Unplugged: the default stands in until it returns.
    QCOMPARE(PipeWireManager::resolveMicTarget("usb_mic", "builtin", {"builtin"}),
             std::string("builtin"));
}

void PipeWireManagerTest::micLeavesABluetoothDefaultAlone() {
    const std::vector<std::string> connected{"bluez_input.headset", "builtin"};
    QCOMPARE(PipeWireManager::resolveMicTarget("", "bluez_input.headset", connected),
             std::string());
    // Asked for by name, it is what was asked for.
    QCOMPARE(PipeWireManager::resolveMicTarget("bluez_input.headset", "builtin", connected),
             std::string("bluez_input.headset"));
}

QTEST_GUILESS_MAIN(PipeWireManagerTest)
#include "PipeWireManagerTest.moc"
