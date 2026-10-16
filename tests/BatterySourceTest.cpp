#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "hid/BatterySource.h"
#include "hid/SysfsBattery.h"

namespace hid = sonar::hid;

// The generic battery path. What matters is that it reads any device the kernel
// reports without a list of products in it, and that it refuses the devices it
// must not speak for — a laptop's own battery being the dangerous one, since
// reporting that as a headset's charge is worse than reporting nothing.
//
// Driven against a fabricated /sys tree rather than the running machine's, so
// the result does not depend on what happens to be plugged in.

class BatterySourceTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void alsaIdFormMatchesHowAlsaBuildsNodeNames();
    void matchesAnAudioNodeToItsOwnUsbDevice();
    void readsCapacityAndChargeState();
    void ignoresTheMachinesOwnBattery();
    void declinesDevicesItCannotSpeakFor();
    void aPresentDeviceWithNoLevelIsStillAnAnswer();
    void matchesADeviceBehindAReceiver();
    void missingPowerSupplyClassIsNotAnError();

private:
    // Builds one power_supply entry whose `device` link resolves to a USB device
    // carrying the given descriptor strings — the shape the kernel produces.
    void makeEntry(const QString& supplyName, const QString& scope,
                   const QString& capacity, const QString& status,
                   const QString& usbDir, const QString& manufacturer,
                   const QString& product);
    static void write(const QString& path, const QString& text);

    QTemporaryDir* tmp_ = nullptr;
    QString supplies_;
};

void BatterySourceTest::write(const QString& path, const QString& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(text.toUtf8());
    f.write("\n");
}

void BatterySourceTest::init() {
    tmp_ = new QTemporaryDir;
    QVERIFY(tmp_->isValid());
    supplies_ = tmp_->filePath(QStringLiteral("class/power_supply"));
    QDir().mkpath(supplies_);
    hid::SysfsBatterySource::setSysfsRoot(supplies_.toStdString());
}

void BatterySourceTest::cleanup() {
    hid::SysfsBatterySource::setSysfsRoot("/sys/class/power_supply");
    delete tmp_;
    tmp_ = nullptr;
}

void BatterySourceTest::makeEntry(const QString& supplyName, const QString& scope,
                                  const QString& capacity, const QString& status,
                                  const QString& usbDir, const QString& manufacturer,
                                  const QString& product) {
    const QString supply = supplies_ + QLatin1Char('/') + supplyName;
    QDir().mkpath(supply);
    if (!scope.isEmpty()) write(supply + QStringLiteral("/scope"), scope);
    if (!capacity.isEmpty()) write(supply + QStringLiteral("/capacity"), capacity);
    if (!status.isEmpty()) write(supply + QStringLiteral("/status"), status);

    if (usbDir.isEmpty()) {
        return;
    }
    // The USB device, and a HID interface under it that the power supply points
    // at — two levels down, so the walk up has to actually happen.
    const QString usb = tmp_->filePath(usbDir);
    QDir().mkpath(usb);
    write(usb + QStringLiteral("/manufacturer"), manufacturer);
    write(usb + QStringLiteral("/product"), product);
    const QString iface = usb + QStringLiteral("/1-2:1.3/hidraw0");
    QDir().mkpath(iface);
    QVERIFY(QFile::link(iface, supply + QStringLiteral("/device")));
}

void BatterySourceTest::alsaIdFormMatchesHowAlsaBuildsNodeNames() {
    // ALSA replaces every run of non-alphanumerics with one underscore, which is
    // what makes a descriptor string comparable to a node name at all.
    QCOMPARE(QString::fromStdString(hid::SysfsBatterySource::alsaIdForm("SteelSeries")),
             QStringLiteral("SteelSeries"));
    QCOMPARE(QString::fromStdString(
                 hid::SysfsBatterySource::alsaIdForm("Arctis Nova Pro Wireless")),
             QStringLiteral("Arctis_Nova_Pro_Wireless"));
    QCOMPARE(QString::fromStdString(hid::SysfsBatterySource::alsaIdForm("HyperX  Cloud II")),
             QStringLiteral("HyperX_Cloud_II"));
    QCOMPARE(QString::fromStdString(hid::SysfsBatterySource::alsaIdForm("  Logitech, Inc. ")),
             QStringLiteral("Logitech_Inc"));
}

void BatterySourceTest::matchesAnAudioNodeToItsOwnUsbDevice() {
    // No product list anywhere: the match is between the node name and the USB
    // strings the kernel read off the device.
    QVERIFY(hid::SysfsBatterySource::nodeMatchesUsb(
        "alsa_output.usb-SteelSeries_Arctis_Nova_Pro_Wireless-00.analog-stereo",
        "SteelSeries", "Arctis Nova Pro Wireless"));
    QVERIFY(hid::SysfsBatterySource::nodeMatchesUsb(
        "alsa_output.usb-Corsair_VIRTUOSO_RGB_Wireless-00.analog-stereo", "Corsair",
        "VIRTUOSO RGB Wireless"));
    QVERIFY(!hid::SysfsBatterySource::nodeMatchesUsb(
        "alsa_output.usb-SteelSeries_Arctis_Nova_Pro_Wireless-00.analog-stereo", "Logitech",
        "G935 Gaming Headset"));
    // A Bluetooth node has no USB strings to match.
    QVERIFY(!hid::SysfsBatterySource::nodeMatchesUsb("bluez_output.80_C3_BA_94_FA_B8.1",
                                                     "SteelSeries", "Arctis Nova Pro"));
}

void BatterySourceTest::readsCapacityAndChargeState() {
    makeEntry(QStringLiteral("hid-headset-battery"), QStringLiteral("Device"),
              QStringLiteral("72"), QStringLiteral("Discharging"),
              QStringLiteral("devices/usb1/1-2"), QStringLiteral("Corsair"),
              QStringLiteral("VIRTUOSO RGB Wireless"));

    hid::SysfsBatterySource source;
    const auto reading =
        source.read("alsa_output.usb-Corsair_VIRTUOSO_RGB_Wireless-00.analog-stereo");
    QVERIFY2(reading.has_value(), "the generic source did not recognise its own device");
    QCOMPARE(reading->percent, 72);
    QVERIFY(reading->valid());
    QVERIFY(!reading->charging());
    QCOMPARE(reading->state, hid::ChargeState::Discharging);
}

void BatterySourceTest::ignoresTheMachinesOwnBattery() {
    // scope=System is the laptop's own pack. Reporting that as a headset's
    // charge would be confidently wrong, which is worse than silent.
    makeEntry(QStringLiteral("BAT0"), QStringLiteral("System"), QStringLiteral("91"),
              QStringLiteral("Discharging"), QStringLiteral("devices/usb1/1-2"),
              QStringLiteral("Corsair"), QStringLiteral("VIRTUOSO RGB Wireless"));

    hid::SysfsBatterySource source;
    QVERIFY(!source
                 .read("alsa_output.usb-Corsair_VIRTUOSO_RGB_Wireless-00.analog-stereo")
                 .has_value());
}

void BatterySourceTest::declinesDevicesItCannotSpeakFor() {
    makeEntry(QStringLiteral("hid-mouse-battery"), QStringLiteral("Device"),
              QStringLiteral("55"), QStringLiteral("Discharging"),
              QStringLiteral("devices/usb1/1-4"), QStringLiteral("Logitech"),
              QStringLiteral("G305 Lightspeed Wireless Gaming Mouse"));

    hid::SysfsBatterySource source;
    // A mouse's battery is not the headset's. Declining is the whole point of
    // returning nullopt rather than a reading.
    QVERIFY(!source
                 .read("alsa_output.usb-SteelSeries_Arctis_Nova_Pro_Wireless-00.analog-stereo")
                 .has_value());
    // ...and it does answer for its own device.
    QVERIFY(source
                .read("alsa_output.usb-Logitech_G305_Lightspeed_Wireless_Gaming_Mouse-00")
                .has_value());
}

void BatterySourceTest::aPresentDeviceWithNoLevelIsStillAnAnswer() {
    // A headset that is paired but switched off reports no capacity. That is a
    // reading of "unknown", not a reason to fall through to another source which
    // would be guessing.
    makeEntry(QStringLiteral("hid-headset-battery"), QStringLiteral("Device"), QString(),
              QStringLiteral("Unknown"), QStringLiteral("devices/usb1/1-2"),
              QStringLiteral("Corsair"), QStringLiteral("VIRTUOSO RGB Wireless"));

    hid::SysfsBatterySource source;
    const auto reading =
        source.read("alsa_output.usb-Corsair_VIRTUOSO_RGB_Wireless-00.analog-stereo");
    QVERIFY(reading.has_value());
    QCOMPARE(reading->percent, -1);
    QVERIFY(!reading->valid());
}

void BatterySourceTest::matchesADeviceBehindAReceiver() {
    // The case real hardware exposed: a wireless device on a dongle. The USB
    // descriptors describe the receiver ("USB Receiver"), so matching on those
    // alone finds nothing — the power supply's own model_name is what names the
    // device. Found by probing a Logitech Lightspeed receiver on the author's
    // machine, where the first implementation silently reported no battery.
    const QString supply = supplies_ + QStringLiteral("/hidpp_battery_0");
    QDir().mkpath(supply);
    write(supply + QStringLiteral("/scope"), QStringLiteral("Device"));
    write(supply + QStringLiteral("/capacity"), QStringLiteral("64"));
    write(supply + QStringLiteral("/status"), QStringLiteral("Discharging"));
    write(supply + QStringLiteral("/manufacturer"), QStringLiteral("Logitech"));
    write(supply + QStringLiteral("/model_name"), QStringLiteral("PRO X 2 Wireless Headset"));

    const QString usb = tmp_->filePath(QStringLiteral("devices/usb1/1-9"));
    QDir().mkpath(usb);
    write(usb + QStringLiteral("/manufacturer"), QStringLiteral("Logitech"));
    write(usb + QStringLiteral("/product"), QStringLiteral("USB Receiver"));
    const QString iface = usb + QStringLiteral("/1-9:1.2/0003:046D:C53F.0007");
    QDir().mkpath(iface);
    QVERIFY(QFile::link(iface, supply + QStringLiteral("/device")));

    hid::SysfsBatterySource source;
    const auto reading =
        source.read("alsa_output.usb-Logitech_PRO_X_2_Wireless_Headset-00.analog-stereo");
    QVERIFY2(reading.has_value(), "a device behind a receiver was not recognised");
    QCOMPARE(reading->percent, 64);
}

void BatterySourceTest::missingPowerSupplyClassIsNotAnError() {
    hid::SysfsBatterySource::setSysfsRoot(tmp_->filePath(QStringLiteral("nope")).toStdString());
    hid::SysfsBatterySource source;
    QVERIFY(!source.read("alsa_output.usb-Anything-00").has_value());

    // And the registry copes with having nothing to ask.
    std::vector<std::unique_ptr<hid::IBatterySource>> none;
    QVERIFY(!hid::readBattery(none, "alsa_output.usb-Anything-00").has_value());
}

QTEST_MAIN(BatterySourceTest)
#include "BatterySourceTest.moc"
