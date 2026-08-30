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

// Locate a packaged resource ("udev/70-...rules", "icons/sonero-256.png"),
// searching the AppImage mount, the system prefix, then the source tree.
// Returns an empty string when not found.
[[nodiscard]] QString resourcePath(const QString& relative);

// --- Fixes -------------------------------------------------------------------

// Install the menu entry and icons into ~/.local/share so the AppImage shows up
// like a normal app. User-level, no privileges, idempotent, safe to call on every
// start. Returns true when the entry is present afterwards.
bool installDesktopIntegration();

// The bundle the menu entry currently points at — the one that ran last. Read it
// *before* installDesktopIntegration(), which overwrites that entry.
[[nodiscard]] QString previousBundlePath();

// Deletes `previous`, the bundle superseded by the one running now — updating
// means downloading a new file, so without this every version stays on disk
// forever.
//
// Call it only *after* installDesktopIntegration() has succeeded. Deleting first
// leaves a window in which the launcher still names a file that is already gone,
// and if the rewrite then fails the launcher stays broken for good — the user
// gets "Failed to launch Sonero" and no way to start it from the menu again.
//
// Deliberately narrow, because it removes a file the user downloaded. It acts
// only when: this run is from an AppImage; `previous` differs from the running
// bundle, compared canonically so a symlink or a "../" cannot point back at it;
// and the target is a regular file whose name ends in .AppImage. Anything else is
// left alone. Returns the path removed, or an empty string.
QString removeSupersededBundle(const QString& previous);

// The exact shell commands a privileged fix would run, for display *and* for
// execution — the user sees precisely what will happen before approving.
[[nodiscard]] QString privilegedFixScript(CheckId id);

// Runs the fix through pkexec, which shows the desktop's own authentication
// dialog. Returns false if the user cancelled or the command failed; `error`
// receives a human-readable reason.
bool runPrivilegedFix(CheckId id, QString* error = nullptr);

}  // namespace sonar::setup
