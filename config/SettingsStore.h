#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "dsp/BassStage.h"
#include "dsp/Equalizer.h"
#include "dsp/MicChain.h"

class QTimer;

namespace sonar::config {

// User settings in ~/.config/Sonero/settings.json, one JSON object per section.
// Writes are debounced; a missing or damaged file is a first run.
class SettingsStore : public QObject {
    Q_OBJECT

public:
    explicit SettingsStore(QObject* parent = nullptr);
    ~SettingsStore() override;

    // True when no (valid) settings file existed at construction.
    [[nodiscard]] bool isFirstRun() const { return firstRun_; }

    // A named top-level section, or an empty object when absent.
    [[nodiscard]] QJsonObject section(const QString& key) const;

    // Replace a section and schedule a debounced write to disk.
    void putSection(const QString& key, const QJsonObject& value);

    // One key of a section (Undefined when absent), and its read-modify-write.
    [[nodiscard]] QJsonValue value(const QString& section, const QString& key) const;
    void putValue(const QString& section, const QString& key, const QJsonValue& value);

    // Write immediately (also performed on destruction).
    void flush();

    [[nodiscard]] static QString filePath();

private:
    // Bring settings over from the pre-rename location, once.
    static void migrateFromLegacyLocation();

    QJsonObject root_;
    QTimer* saveTimer_ = nullptr;
    bool firstRun_ = true;
    bool dirty_ = false;
};

// EqSettings <-> JSON. `eqFromJson` returns `fallback` when no bands are stored, and
// keeps the band count consistent with the bands read.
QJsonObject eqToJson(const dsp::EqSettings& settings);
dsp::EqSettings eqFromJson(const QJsonObject& obj, const dsp::EqSettings& fallback);

// MicSettings <-> JSON, missing keys from `fallback`, values clamped: the file can
// be hand-edited, and a ratio of zero divides by zero on the audio thread.
QJsonObject micToJson(const dsp::MicSettings& settings);
dsp::MicSettings micFromJson(const QJsonObject& obj, const dsp::MicSettings& fallback);

// BassStageSettings (harmonic bass + output limiter) <-> JSON. Anything missing
// or of the wrong type is taken from `fallback`.
QJsonObject bassToJson(const dsp::BassStageSettings& settings);
dsp::BassStageSettings bassFromJson(const QJsonObject& obj,
                                    const dsp::BassStageSettings& fallback);

}  // namespace sonar::config
