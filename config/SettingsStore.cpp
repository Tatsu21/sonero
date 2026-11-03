#include "config/SettingsStore.h"

#include <algorithm>
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

QJsonObject micToJson(const dsp::MicSettings& s) {
    QJsonObject o;
    o[QStringLiteral("inputGainDb")] = s.inputGainDb;
    o[QStringLiteral("outputGainDb")] = s.outputGainDb;
    o[QStringLiteral("muted")] = s.muted;
    o[QStringLiteral("noiseSuppression")] = s.noiseSuppression;
    o[QStringLiteral("highPass")] = QJsonObject{
        {QStringLiteral("enabled"), s.highPass.enabled},
        {QStringLiteral("frequencyHz"), s.highPass.frequencyHz},
    };
    o[QStringLiteral("gate")] = QJsonObject{
        {QStringLiteral("enabled"), s.gate.enabled},
        {QStringLiteral("thresholdDb"), s.gate.thresholdDb},
        {QStringLiteral("ratio"), s.gate.ratio},
        {QStringLiteral("attackMs"), s.gate.attackMs},
        {QStringLiteral("holdMs"), s.gate.holdMs},
        {QStringLiteral("releaseMs"), s.gate.releaseMs},
    };
    o[QStringLiteral("deEsser")] = QJsonObject{
        {QStringLiteral("enabled"), s.deEsser.enabled},
        {QStringLiteral("frequencyHz"), s.deEsser.frequencyHz},
        {QStringLiteral("thresholdDb"), s.deEsser.thresholdDb},
        {QStringLiteral("ratio"), s.deEsser.ratio},
    };
    o[QStringLiteral("compressor")] = QJsonObject{
        {QStringLiteral("enabled"), s.compressor.enabled},
        {QStringLiteral("thresholdDb"), s.compressor.thresholdDb},
        {QStringLiteral("ratio"), s.compressor.ratio},
        {QStringLiteral("kneeDb"), s.compressor.kneeDb},
        {QStringLiteral("attackMs"), s.compressor.attackMs},
        {QStringLiteral("releaseMs"), s.compressor.releaseMs},
        {QStringLiteral("autoMakeup"), s.compressor.autoMakeup},
        {QStringLiteral("makeupDb"), s.compressor.makeupDb},
    };
    o[QStringLiteral("limiter")] = QJsonObject{
        {QStringLiteral("enabled"), s.limiter.enabled},
        {QStringLiteral("ceilingDb"), s.limiter.ceilingDb},
        {QStringLiteral("releaseMs"), s.limiter.releaseMs},
    };
    return o;
}

dsp::MicSettings micFromJson(const QJsonObject& obj, const dsp::MicSettings& fallback) {
    const auto number = [](const QJsonObject& o, const char* key, float current, float min,
                           float max) {
        const double v = o.value(QLatin1String(key)).toDouble(static_cast<double>(current));
        return std::clamp(static_cast<float>(v), min, max);
    };
    const auto flag = [](const QJsonObject& o, const char* key, bool current) {
        return o.value(QLatin1String(key)).toBool(current);
    };

    dsp::MicSettings s = fallback;
    s.inputGainDb = number(obj, "inputGainDb", s.inputGainDb, -12.0f, 24.0f);
    s.outputGainDb = number(obj, "outputGainDb", s.outputGainDb, -12.0f, 12.0f);
    s.muted = flag(obj, "muted", s.muted);
    s.noiseSuppression = flag(obj, "noiseSuppression", s.noiseSuppression);

    const QJsonObject highPass = obj.value(QStringLiteral("highPass")).toObject();
    s.highPass.enabled = flag(highPass, "enabled", s.highPass.enabled);
    s.highPass.frequencyHz = number(highPass, "frequencyHz", s.highPass.frequencyHz, 20.0f, 300.0f);

    const QJsonObject gate = obj.value(QStringLiteral("gate")).toObject();
    s.gate.enabled = flag(gate, "enabled", s.gate.enabled);
    s.gate.thresholdDb = number(gate, "thresholdDb", s.gate.thresholdDb, -80.0f, 0.0f);
    s.gate.ratio = number(gate, "ratio", s.gate.ratio, 1.0f, 10.0f);
    s.gate.attackMs = number(gate, "attackMs", s.gate.attackMs, 0.0f, 50.0f);
    s.gate.holdMs = number(gate, "holdMs", s.gate.holdMs, 0.0f, 500.0f);
    s.gate.releaseMs = number(gate, "releaseMs", s.gate.releaseMs, 10.0f, 1000.0f);

    const QJsonObject deEsser = obj.value(QStringLiteral("deEsser")).toObject();
    s.deEsser.enabled = flag(deEsser, "enabled", s.deEsser.enabled);
    s.deEsser.frequencyHz = number(deEsser, "frequencyHz", s.deEsser.frequencyHz, 3000.0f, 10000.0f);
    s.deEsser.thresholdDb = number(deEsser, "thresholdDb", s.deEsser.thresholdDb, -60.0f, 0.0f);
    s.deEsser.ratio = number(deEsser, "ratio", s.deEsser.ratio, 1.0f, 10.0f);

    const QJsonObject comp = obj.value(QStringLiteral("compressor")).toObject();
    s.compressor.enabled = flag(comp, "enabled", s.compressor.enabled);
    s.compressor.thresholdDb = number(comp, "thresholdDb", s.compressor.thresholdDb, -60.0f, 0.0f);
    s.compressor.ratio = number(comp, "ratio", s.compressor.ratio, 1.0f, 10.0f);
    s.compressor.kneeDb = number(comp, "kneeDb", s.compressor.kneeDb, 0.0f, 12.0f);
    s.compressor.attackMs = number(comp, "attackMs", s.compressor.attackMs, 0.0f, 100.0f);
    s.compressor.releaseMs = number(comp, "releaseMs", s.compressor.releaseMs, 10.0f, 500.0f);
    s.compressor.autoMakeup = flag(comp, "autoMakeup", s.compressor.autoMakeup);
    s.compressor.makeupDb = number(comp, "makeupDb", s.compressor.makeupDb, 0.0f, 24.0f);

    const QJsonObject limiter = obj.value(QStringLiteral("limiter")).toObject();
    s.limiter.enabled = flag(limiter, "enabled", s.limiter.enabled);
    s.limiter.ceilingDb = number(limiter, "ceilingDb", s.limiter.ceilingDb, -12.0f, 0.0f);
    s.limiter.releaseMs = number(limiter, "releaseMs", s.limiter.releaseMs, 10.0f, 500.0f);
    return s;
}

}  // namespace sonar::config
