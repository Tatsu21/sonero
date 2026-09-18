# Installing Sonero

Three ways to install, depending on what you want:

| | `.deb` | pacman package | AppImage |
|---|---|---|---|
| Distributions | Debian / Ubuntu | Arch and derivatives | any |
| Needs root | yes (`apt`) | yes (`pacman`) | no |
| Dependencies | installed by apt | installed by pacman | bundled |
| SteelSeries headset access | **works immediately** | **works immediately** | needs one click in Settings |
| Removing it | `sudo apt remove sonero` | `sudo pacman -R sonero` | delete the file |

**Prefer your distribution's package** — the `.deb` on Debian or Ubuntu, the
`PKGBUILD` in this repository on Arch. Both set everything up, including the udev
rule that a portable bundle cannot install on its own.

## The Debian package

```sh
sudo apt install ./sonero_0.1.0_amd64.deb
```

apt pulls in Qt and the rest. The package also installs the udev rule granting
your user access to SteelSeries HID devices and reloads udev, so headset battery
level works right away — no extra steps.

PipeWire itself is a *recommended* dependency rather than a hard one (it is
already running on any current desktop), so installation never drags in an audio
server you did not ask for.

### A .deb only fits the distribution it was built on

The package links against the distribution's own Qt, so `apt` will refuse a
package built elsewhere:

```
sonero depends on libqt6core6t64 (>= 6.8.2); however:
  Version of libqt6core6t64 on system is 6.4.2.
```

That is not a packaging bug — a `.deb` cannot carry its own Qt. Build one per
target:

| Target | Build on | Qt |
|---|---|---|
| Linux Mint 21.x, Ubuntu 22.04 | Ubuntu 22.04 | 6.2 |
| Linux Mint 22.x, Ubuntu 24.04 | Ubuntu 24.04 | 6.4 |
| Debian 13 | Debian 13 | 6.8 |

Build for another distribution with Docker — this also **installs the result in a
clean container** to prove `apt` accepts it, which inspecting the file cannot tell
you:

```sh
./packaging/deb/build-deb-docker.sh                 # Ubuntu 24.04 / Mint 22
./packaging/deb/build-deb-docker.sh ubuntu:22.04    # Ubuntu 22.04 / Mint 21
./packaging/deb/build-deb-docker.sh debian:13       # Debian 13 (trixie)
```

Packages land in `dist/<image-tag>/`, so builds for several targets coexist.

CI builds all three on every push (see `.github/workflows/ci.yml`); download
the artifact matching the target machine. To build for the distribution you are
already on:

```sh
./packaging/deb/build-deb.sh
```

**If you cannot build for someone else's distribution, give them the AppImage** —
it bundles Qt and runs anywhere.

The packaged build deliberately links no spdlog: its Debian package name encodes
both its own and libfmt's ABI version (`libspdlog1.15-fmt10`), which exists on no
other distribution. Sonero falls back to its built-in logger, leaving only Qt,
PipeWire and libc as dependencies.

# The pacman package (Arch and derivatives)

The package is not on the AUR yet: registrations there are closed at the time of
writing, so it cannot be uploaded. The `PKGBUILD` is finished, though, and builds
the same package the AUR would serve — you just point your helper at this
repository instead of at a package name:

```sh
git clone https://github.com/Tatsu21/sonero.git
yay -B sonero/packaging/aur/sonero        # paru -B works the same way
```

`-B` builds a `PKGBUILD` from a directory: it fetches the release tarball, pulls
in the build dependencies, compiles against your Qt and PipeWire, runs the test
suite and hands the finished package to pacman. Afterwards it is an ordinary
package — it shows up in `pacman -Qi sonero` and leaves with `pacman -R sonero`.

Without a helper:

```sh
makepkg -si -D packaging/aur/sonero
```

Two directories to choose from:

| Directory | Builds |
|---|---|
| `packaging/aur/sonero` | the newest release |
| `packaging/aur/sonero-git` | `main` as it stands |

To update, `git pull` and run the same command again. Once the AUR is open for
registrations this becomes `yay -S sonero`, and upgrades come with the rest of
your system.

A headset that was already plugged in when you installed picks up the new udev
permissions after a replug or a reboot; the package triggers udev, but an
in-flight device keeps the access it was opened with.

PipeWire and WirePlumber are `optdepends`, not `depends`: they are the audio
server Sonero drives rather than libraries it links, and installing a mixer
should not pull an audio server onto a machine running something else. Every
current Arch desktop already has both.

# The AppImage

A single portable file. There is no installer, and nothing is written outside your
home directory unless you explicitly approve it.

## Run it

```sh
chmod +x Sonero-*-x86_64.AppImage
./Sonero-*-x86_64.AppImage
```

On the first run Sonero adds itself to your application menu (a `.desktop`
entry and icons under `~/.local/share`). That step needs no privileges and is
repeated silently on every start, so moving the AppImage elsewhere keeps the menu
entry pointing at the right place.

## Requirements

| Needed | Why | If missing |
|---|---|---|
| **PipeWire** | Sonero mixes and routes through it | Install it with your package manager — every current distribution ships it |
| FUSE 2 | how AppImages mount themselves | `sudo apt install libfuse2` (Debian/Ubuntu), or run with `--appimage-extract-and-run` |

Both are already present on a normal desktop install.

## Keeping Sonero up to date

**Settings → Updates** holds one switch, off until you turn it on: *Check for
updates automatically*. With it on, Sonero asks GitHub once a day whether a newer
release exists and says so; **Check now** does the same on demand. Nothing is
downloaded or installed until you press a button.

What that button does depends on how Sonero got onto the machine — it works this
out from where it is running, not from a setting:

| Installed as | The button |
|---|---|
| pacman package | Copies `yay -Syu sonero`. Sonero does not touch files pacman owns. |
| `.deb` | Copies the `apt install` line and links the release page. |
| AppImage | Downloads the new bundle next to the current one, then restarts into it. |
| Local build | Links the release page; updating is `git pull` and a rebuild. |

For the AppImage this is a real self-update: the download lands beside the bundle
you are running (never over it — the file is mounted for as long as the app is
open), it has to be an AppImage or it is thrown away, and the old bundle is
removed by the new one on its first start.

The check is a single HTTPS request to `api.github.com` carrying no identity and
no telemetry. [SECURITY.md](../SECURITY.md#the-update-check) spells out the trust
model, including what it does *not* give you: releases are not signed.

## Optional: hardware features that need administrator rights

Open **Settings → System integration**. Each row shows what is set up and what is
not. Rows marked with a lock change system files; clicking *Set up…* first shows
you the **exact commands**, then your desktop asks for your password. Nothing runs
if you cancel.

### SteelSeries headset access
Reading the battery level of a SteelSeries headset means talking to `/dev/hidraw*`,
which is root-only by default. The fix installs a udev rule granting the logged-in
user access to SteelSeries devices only:

```sh
install -m 0644 70-sonero-steelseries.rules /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=hidraw
```

Undo it by deleting `/etc/udev/rules.d/70-sonero-steelseries.rules`.

#### My headset is not the one Sonero knows about

Battery level speaks a protocol that differs per model, and Sonero currently ships only
the SteelSeries Arctis Nova Pro Wireless. Every other headset still plays audio
perfectly — routing, volume, the equalizer and the mixer need no HID at all — it is
only the battery percentage that is model-specific.

**You do not have to read the code to add yours.** The repository carries a written
procedure at [`hid/SKILL.md`](../hid/SKILL.md), meant to be handed to an AI assistant
(Claude Code, or any model that can read a repository) so it can do the work for you:
it walks through identifying the device, granting it access, sourcing the protocol
from an existing implementation, writing a new device class next to the existing one,
and — the part that matters — how to prove the result is real rather than plausible.
If you use Claude Code, the same procedure is registered as the `add-hid-headset`
skill, so `/add-hid-headset` starts it.

It covers battery level and stops there. Sonero can also send an equalizer to the
Arctis's onboard DSP, but the headset accepts the write without any audible change and
nobody has worked out why — so the procedure tells an assistant not to attempt onboard
EQ, sidetone or ANC for a new model, since there is no working example to copy.

What is asked of you is the physical half an assistant cannot do: plugging the headset
in, turning it off and on when asked, and saying whether the number it produces
matches what your headset actually reports. If it works, a pull request is welcome.

### Bluetooth battery reporting
BlueZ only exposes battery level with its experimental interfaces enabled. The fix
adds a systemd drop-in (it does **not** edit the distribution's own unit or
`main.conf`) and restarts the Bluetooth service — connected devices drop for a
moment:

```sh
/etc/systemd/system/bluetooth.service.d/10-sonero.conf
```

Undo it by deleting that file and running `systemctl daemon-reload && systemctl
restart bluetooth`.

## Start automatically at login

**Settings → Background → "Start Sonero when I log in"** writes
`~/.config/autostart/Sonero.desktop`, which launches the app hidden
(`--background`) so its audio routing is live without a window in your face.

Note that closing the window keeps Sonero running in the background — its
virtual channels disappear from other apps if the process exits. Use
**Settings → Background → Quit Sonero** (or the tray icon, where your desktop
has one) to exit fully. Launching the AppImage again brings the window back
instead of starting a second copy.

> **GNOME note:** GNOME has no system tray by default, so no tray icon appears.
> Everything else works; reopen the window by launching Sonero again.

## Where settings live

```
~/.config/Sonero/Sonero/settings.json   mixer, EQ, routing, preferences
~/.config/Sonero/Sonero/presets/        saved equalizer presets
```

Delete that directory to reset Sonero to defaults.
