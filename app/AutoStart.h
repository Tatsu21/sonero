#pragma once

#include <QString>

namespace sonar::autostart {

// "Start Sonero when you log in", implemented with the XDG Desktop Application
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

// The program path recorded in a desktop entry's Exec line, unquoted. Empty when
// the file is unreadable or the line is not in the quoted form written here.
// Shared with the AppImage bookkeeping in SystemSetup, which reads the menu
// entry to learn which bundle ran last.
[[nodiscard]] QString execPathIn(const QString& desktopFile);

// Creates or removes the autostart entry. Returns false if the file could not be
// written or removed.
bool setEnabled(bool on);

// Keeps an existing entry pointing at the program running now.
//
// An AppImage carries its own path, so replacing the bundle with a newer one —
// a different file name, usually a different directory — leaves login starting
// the old bundle, or nothing at all once it is deleted. Rewrites the entry when
// the recorded path differs from this one AND either this run came from an
// AppImage (the update case) or the recorded program no longer exists (the entry
// is dead anyway). A one-off run from a source tree therefore cannot hijack an
// installed app's login entry. Returns true when the entry was rewritten.
bool refreshExecPath();

}  // namespace sonar::autostart
