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

QTEST_GUILESS_MAIN(PipeWireManagerTest)
#include "PipeWireManagerTest.moc"
