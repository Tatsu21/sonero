#include "config/SettingsStore.h"

#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace sonar::config {

namespace {
constexpr int kSaveDebounceMs = 400;
}  // namespace

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {
    migrateFromLegacyLocation();
    QFile file(filePath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            root_ = doc.object();
            firstRun_ = false;
        }
    }
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kSaveDebounceMs);
    connect(saveTimer_, &QTimer::timeout, this, &SettingsStore::flush);
}

SettingsStore::~SettingsStore() { flush(); }

// The project was called LinuxSonar before, which put the settings under a
// different application directory. Carry them over once, so renaming the app does
// not silently reset everybody's mixer, EQ and routing.
void SettingsStore::migrateFromLegacyLocation() {
    const QString current = filePath();
    if (QFileInfo::exists(current)) {
        return;  // already migrated, or a genuine fresh start
    }
    const QString legacy =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/LinuxSonar/LinuxSonar/settings.json");
    if (!QFileInfo::exists(legacy)) {
        return;
    }
    QDir().mkpath(QFileInfo(current).absolutePath());
    if (QFile::copy(legacy, current)) {
        // Copy, not move: if anything about the new location is wrong the old
        // settings are still there to fall back on.
        QFile::copy(QFileInfo(legacy).absolutePath() + QStringLiteral("/presets"),
                    QFileInfo(current).absolutePath() + QStringLiteral("/presets"));
    }
}

QString SettingsStore::filePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/settings.json");
}

QJsonObject SettingsStore::section(const QString& key) const {
    return root_.value(key).toObject();
}

void SettingsStore::putSection(const QString& key, const QJsonObject& value) {
    root_[key] = value;
    dirty_ = true;
    saveTimer_->start();
}

void SettingsStore::flush() {
    if (!dirty_) {
        return;
    }
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    root_[QStringLiteral("app")] = QStringLiteral("Sonero");
    root_[QStringLiteral("kind")] = QStringLiteral("settings");
    root_[QStringLiteral("version")] = 1;
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
        if (file.commit()) {
            dirty_ = false;
        }
    }
}

QJsonObject eqToJson(const dsp::EqSettings& settings) {
    QJsonArray bands;
    for (const dsp::EqBand& b : settings.bands) {
        QJsonObject bo;
        bo[QStringLiteral("f")] = b.frequency;
        bo[QStringLiteral("g")] = b.gainDb;
        bands.append(bo);
    }
    QJsonObject o;
    o[QStringLiteral("enabled")] = settings.enabled;
    o[QStringLiteral("preset")] = static_cast<int>(settings.preset);
    o[QStringLiteral("bands")] = bands;
    return o;
}

dsp::EqSettings eqFromJson(const QJsonObject& obj, const dsp::EqSettings& fallback) {
    if (!obj.contains(QStringLiteral("bands"))) {
        return fallback;
    }
    std::vector<dsp::EqBand> bands;
    for (const QJsonValue v : obj.value(QStringLiteral("bands")).toArray()) {
        const QJsonObject bo = v.toObject();
        bands.push_back(
            dsp::EqBand{static_cast<float>(bo.value(QStringLiteral("f")).toDouble()),
                        static_cast<float>(bo.value(QStringLiteral("g")).toDouble())});
    }
    if (bands.empty()) {
        return fallback;
    }
    dsp::EqSettings s = fallback;
    s.enabled = obj.value(QStringLiteral("enabled")).toBool(fallback.enabled);
    s.preset = static_cast<dsp::EqPreset>(
        obj.value(QStringLiteral("preset")).toInt(static_cast<int>(fallback.preset)));
    s.bands = std::move(bands);
    switch (s.bands.size()) {  // keep bandCount consistent with the stored bands
        case 15: s.bandCount = dsp::BandCount::Bands15; break;
        case 31: s.bandCount = dsp::BandCount::Bands31; break;
        default: s.bandCount = dsp::BandCount::Bands10; break;
    }
    return s;
}

}  // namespace sonar::config
