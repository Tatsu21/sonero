#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <QString>

#include "audio/Channel.h"

class QBoxLayout;
class QComboBox;
class QLabel;
class QLayout;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace sonar::ui {

// Building blocks every page is made of, spelled once so the pages cannot drift
// apart in margins, object names or spacing.

[[nodiscard]] inline QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// A channel's name as the settings file keys it ("Game", "Chat"…).
[[nodiscard]] QString channelKey(audio::ChannelId id);

// A label carrying one of the application sheet's object names ("Hint", "CardKey"…).
[[nodiscard]] QLabel* makeLabel(const QString& text, const char* objectName,
                                bool wordWrap = false);

// The right-aligned numeric readout beside a slider or a meter.
[[nodiscard]] QLabel* makeValueLabel(const QString& text);

// A push button with the pointing-hand cursor every clickable button here uses.
[[nodiscard]] QPushButton* makeButton(const QString& text);

// A 0..100 bar with no text inside it; the number, if any, goes in a value label.
[[nodiscard]] QProgressBar* makeLevelBar(const char* objectName);

// Make `owner` a borderless scroll area and return the layout of the page
// inside it, with the standard page margins.
[[nodiscard]] QVBoxLayout* makeScrollPage(QWidget* owner, int spacing);

// Page title + subtitle, stacked, added to `root`.
void addPageHeader(QBoxLayout* root, const QString& title, const QString& subtitle);

// A titled card added to `root`; returns the body layout to fill. `badge`, when
// given, is shown at the right of the title (the "PREPARED" tag).
QVBoxLayout* makeCard(QBoxLayout* root, const QString& title, const QString& subtitle = {},
                      int spacing = 8, const QString& badge = {});

// Refill a device picker silently: `defaultText` (data ""), then (text, node name)
// items. A missing `selected` is kept as `missingText` rather than replaced.
void fillDeviceCombo(QComboBox* combo, const QString& defaultText,
                     const std::vector<std::pair<QString, QString>>& items,
                     const QString& selected, const QString& missingText = {});

// Delete every child widget and nested layout so a container can be refilled.
void clearLayout(QLayout* layout);

}  // namespace sonar::ui
