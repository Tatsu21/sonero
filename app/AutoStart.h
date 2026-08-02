#pragma once

#include <QString>

namespace sonar::autostart {

// "Start LinuxSonar when you log in", implemented with the XDG Desktop Application
// Autostart spec: a .desktop entry in ~/.config/autostart that every major desktop
// (GNOME, KDE, XFCE, …) launches at session start.
//
// The entry launches the app with --background so it starts hidden as a daemon
// rather than popping a window in the user's face on every login.

// Absolute path of the autostart entry we manage.
[[nodiscard]] QString desktopFilePath();

// The command the entry should run. Prefers $APPIMAGE (so an AppImage points at
// itself rather than at its temporary mount) and falls back to the running binary.
[[nodiscard]] QString executablePath();

[[nodiscard]] bool isEnabled();

// Creates or removes the autostart entry. Returns false if the file could not be
// written or removed.
bool setEnabled(bool on);

}  // namespace sonar::autostart
