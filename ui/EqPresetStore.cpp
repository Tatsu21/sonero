#include "ui/EqPresetStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace sonar::ui::presets {

namespace {

QString sanitize(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        out.append(c.isLetterOrNumber() || c == '-' || c == '_' ? c : QChar('_'));
    }
    return out.isEmpty() ? QStringLiteral("preset") : out;
}

QString pathFor(const QString& name) {
    return directory() + '/' + sanitize(name) + QStringLiteral(".json");
}

dsp::BandCount toBandCount(int n) {
    switch (n) {
        case 15: return dsp::BandCount::Bands15;
        case 31: return dsp::BandCount::Bands31;
        default: return dsp::BandCount::Bands10;
    }
}

QJsonObject toJson(const QString& name, const dsp::EqSettings& s) {
    QJsonArray bands;
    for (const dsp::EqBand& b : s.bands) {
        QJsonObject bo;
        bo[QStringLiteral("f")] = b.frequency;
        bo[QStringLiteral("g")] = b.gainDb;
        bands.append(bo);
    }
    QJsonObject o;
    o[QStringLiteral("app")] = QStringLiteral("LinuxSonar");
    o[QStringLiteral("kind")] = QStringLiteral("equalizer-preset");
    o[QStringLiteral("version")] = 1;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("bandCount")] = static_cast<int>(s.bandCount);
    o[QStringLiteral("enabled")] = s.enabled;
    o[QStringLiteral("bands")] = bands;
    return o;
}

std::optional<dsp::EqSettings> fromJson(const QJsonObject& o, QString* nameOut) {
    if (!o.contains(QStringLiteral("bands"))) {
        return std::nullopt;
    }
    dsp::EqSettings s;
    s.preset = dsp::EqPreset::Custom;
    s.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    s.bandCount = toBandCount(o.value(QStringLiteral("bandCount")).toInt(10));
    for (const QJsonValue v : o.value(QStringLiteral("bands")).toArray()) {
        const QJsonObject bo = v.toObject();
        s.bands.push_back(dsp::EqBand{static_cast<float>(bo.value(QStringLiteral("f")).toDouble()),
                                      static_cast<float>(bo.value(QStringLiteral("g")).toDouble())});
    }
    if (s.bands.empty()) {
        return std::nullopt;
    }
    if (nameOut != nullptr) {
        *nameOut = o.value(QStringLiteral("name")).toString();
    }
    return s;
}

std::optional<QJsonObject> readJson(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

bool writeJson(const QString& filePath, const QJsonObject& obj) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

}  // namespace

QString directory() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
        QStringLiteral("/presets");
    QDir().mkpath(dir);
    return dir;
}

QStringList userPresetNames() {
    QStringList names;
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        if (const auto obj = readJson(dir.filePath(file))) {
            const QString name = obj->value(QStringLiteral("name")).toString();
            if (!name.isEmpty()) {
                names << name;
            }
        }
    }
    return names;
}

bool save(const QString& name, const dsp::EqSettings& settings) {
    return writeJson(pathFor(name), toJson(name, settings));
}

std::optional<dsp::EqSettings> load(const QString& name) {
    const auto obj = readJson(pathFor(name));
    if (!obj) {
        return std::nullopt;
    }
    return fromJson(*obj, nullptr);
}

bool remove(const QString& name) { return QFile::remove(pathFor(name)); }

std::optional<QString> importFile(const QString& filePath) {
    const auto obj = readJson(filePath);
    if (!obj) {
        return std::nullopt;
    }
    QString name;
    const auto settings = fromJson(*obj, &name);
    if (!settings) {
        return std::nullopt;
    }
    if (name.isEmpty()) {
        name = QFileInfo(filePath).completeBaseName();
    }
    if (!save(name, *settings)) {
        return std::nullopt;
    }
    return name;
}

bool exportFile(const QString& filePath, const QString& name, const dsp::EqSettings& settings) {
    return writeJson(filePath, toJson(name, settings));
}

}  // namespace sonar::ui::presets
