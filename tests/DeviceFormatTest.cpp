#include <QtTest>

#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "audio/DeviceFormat.h"

using sonar::audio::OutputFormat;
using sonar::audio::OutputFormatManager;

// Verifies the WirePlumber drop-in generation and parsing. No PipeWire /
// WirePlumber daemon is required: renderConfig() is pure and current() reads a
// file from a redirected XDG_CONFIG_HOME.
class DeviceFormatTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void rendersBitDepthAsSpaFormat();
    void rendersRateAndNodePattern();
    void currentIsEmptyWithoutFile();
    void currentRoundTripsWrittenConfig();

private:
    std::filesystem::path tmp_;
};

void DeviceFormatTest::initTestCase() {
    tmp_ = std::filesystem::temp_directory_path() /
           ("sonero-devfmt-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp_);
    qputenv("XDG_CONFIG_HOME", QByteArray::fromStdString(tmp_.string()));
}

void DeviceFormatTest::rendersBitDepthAsSpaFormat() {
    OutputFormatManager mgr;
    QVERIFY(mgr.renderConfig({48000, 16}).find("\"S16LE\"") != std::string::npos);
    QVERIFY(mgr.renderConfig({48000, 24}).find("\"S24LE\"") != std::string::npos);
}

void DeviceFormatTest::rendersRateAndNodePattern() {
    OutputFormatManager mgr("~alsa_output.usb-Test.*");
    const std::string cfg = mgr.renderConfig({96000, 24});
    QVERIFY(cfg.find("audio.rate = 96000") != std::string::npos);
    QVERIFY(cfg.find("~alsa_output.usb-Test.*") != std::string::npos);
    QVERIFY(cfg.find("monitor.alsa.rules") != std::string::npos);
}

void DeviceFormatTest::currentIsEmptyWithoutFile() {
    OutputFormatManager mgr;
    std::filesystem::remove(mgr.configPath());
    QVERIFY(!mgr.current().has_value());
}

void DeviceFormatTest::currentRoundTripsWrittenConfig() {
    OutputFormatManager mgr;
    const OutputFormat want{96000, 16};

    const std::filesystem::path path(mgr.configPath());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << mgr.renderConfig(want);

    const auto got = mgr.current();
    QVERIFY(got.has_value());
    QCOMPARE(got->rateHz, 96000);
    QCOMPARE(got->bitDepth, 16);
    QVERIFY(*got == want);
}

QTEST_GUILESS_MAIN(DeviceFormatTest)
#include "DeviceFormatTest.moc"
