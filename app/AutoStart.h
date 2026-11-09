#pragma once

#include <QString>

namespace sonar::autostart {

// "Start Sonero when you log in": an XDG autostart .desktop entry that launches
// the app with --background, so it starts hidden.

// Absolute path of the autostart entry we manage.
[[nodiscard]] QString desktopFilePath();

// The command the entry should run. Prefers $APPIMAGE (so an AppImage points at
// itself rather than at its temporary mount) and falls back to the running binary.
[[nodiscard]] QString executablePath();

[[nodiscard]] bool isEnabled();

// The unquoted program path from a desktop entry's Exec line; empty when unreadable.
// Also used by SystemSetup to learn which bundle ran last.
[[nodiscard]] QString execPathIn(const QString& desktopFile);

// Creates or removes the autostart entry. Returns false if the file could not be
// written or removed.
bool setEnabled(bool on);

// Re-points an existing entry at the running program when the recorded path differs
// AND (this run is an AppImage OR the recorded program is gone) — so a source-tree
// run cannot hijack an installed app's entry. Returns true when rewritten.
bool refreshExecPath();

}  // namespace sonar::autostart
