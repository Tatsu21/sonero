#pragma once

#include <optional>

#include <QString>
#include <QStringList>

#include "dsp/Equalizer.h"

// Persistence for user equalizer presets as JSON files under the app config dir.
namespace sonar::ui::presets {

// Directory where user presets live (created on demand).
QString directory();

// Display names of all saved user presets.
QStringList userPresetNames();

bool save(const QString& name, const dsp::EqSettings& settings);
std::optional<dsp::EqSettings> load(const QString& name);
bool remove(const QString& name);

// Import a .json preset file into the store; returns the preset name on success.
std::optional<QString> importFile(const QString& filePath);

// Export the given settings to an arbitrary .json path.
bool exportFile(const QString& filePath, const QString& name, const dsp::EqSettings& settings);

}  // namespace sonar::ui::presets
