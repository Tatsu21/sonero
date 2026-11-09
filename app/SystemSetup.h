#pragma once

#include <QString>
#include <vector>

namespace sonar::setup {

// What a check is about. Kept explicit so the UI can order and label them.
enum class CheckId {
    PipeWire,            // the audio server Sonero builds on
    DesktopIntegration,  // menu entry + icon (AppImage only)
    UdevRule,            // /dev/hidraw access for SteelSeries headsets
    BluezBattery,        // BlueZ "Experimental" — Bluetooth battery reporting
};

enum class Status {
    Ok,        // nothing to do
    Missing,   // not set up, and we can offer a fix
    NotNeeded  // irrelevant on this system (e.g. desktop integration when installed)
};

struct Check {
    CheckId id;
    QString title;
    QString detail;       // what it enables, in user terms
    Status status = Status::Ok;
    bool needsRoot = false;
    bool fixable = false;  // false => informational only
};

// Inspect the system. Cheap: only stats files and reads a couple of configs.
[[nodiscard]] std::vector<Check> runChecks();

// True when the process is running from an AppImage bundle.
[[nodiscard]] bool runningFromAppImage();

// Locate a packaged resource in the AppImage mount, the system prefix, then the
// source tree; "" when not found.
[[nodiscard]] QString resourcePath(const QString& relative);

// --- Fixes -------------------------------------------------------------------

// Install the menu entry and icons into ~/.local/share. User-level, idempotent;
// true when the entry is present afterwards.
bool installDesktopIntegration();

// The bundle the menu entry currently points at — the one that ran last. Read it
// *before* installDesktopIntegration(), which overwrites that entry.
[[nodiscard]] QString previousBundlePath();

// Deletes `previous`, the bundle the running one superseded. Call only after
// installDesktopIntegration() succeeded, or the launcher names a missing file.
// Acts only when this run is an AppImage and `previous` is a different (compared
// canonically) regular *.AppImage file. Returns the path removed, or "".
QString removeSupersededBundle(const QString& previous);

// The exact shell commands a privileged fix would run, for display *and* for
// execution — the user sees precisely what will happen before approving.
[[nodiscard]] QString privilegedFixScript(CheckId id);

// Run the fix through pkexec (the desktop's own password dialog). False when
// cancelled or failed, with the reason in `error`.
bool runPrivilegedFix(CheckId id, QString* error = nullptr);

}  // namespace sonar::setup
