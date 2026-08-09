#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "dsp/Equalizer.h"

class QTimer;

namespace sonar::config {

// Persists the app's user settings to a single JSON file under the user's config
// directory (~/.config/Sonero/settings.json). Writes are debounced: a caller
// replaces a named section and the store flushes to disk shortly after, so
// dragging a slider does not hammer the filesystem. A missing or damaged file is
// treated as a first run — callers then keep their built-in defaults.
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

// EqSettings <-> JSON, shared by the software EQ and the headset EQ. `eqFromJson`
// returns `fallback` unchanged when the object carries no bands, and always keeps
// the band count consistent with the number of bands actually stored.
QJsonObject eqToJson(const dsp::EqSettings& settings);
dsp::EqSettings eqFromJson(const QJsonObject& obj, const dsp::EqSettings& fallback);

}  // namespace sonar::config
