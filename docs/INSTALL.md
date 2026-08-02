# Installing LinuxSonar (AppImage)

LinuxSonar ships as a single portable file. There is no installer, and nothing is
written outside your home directory unless you explicitly approve it.

## Run it

```sh
chmod +x LinuxSonar-*-x86_64.AppImage
./LinuxSonar-*-x86_64.AppImage
```

On the first run LinuxSonar adds itself to your application menu (a `.desktop`
entry and icons under `~/.local/share`). That step needs no privileges and is
repeated silently on every start, so moving the AppImage elsewhere keeps the menu
entry pointing at the right place.

## Requirements

| Needed | Why | If missing |
|---|---|---|
| **PipeWire** | LinuxSonar mixes and routes through it | Install it with your package manager — every current distribution ships it |
| FUSE 2 | how AppImages mount themselves | `sudo apt install libfuse2` (Debian/Ubuntu), or run with `--appimage-extract-and-run` |

Both are already present on a normal desktop install.

## Optional: hardware features that need administrator rights

Open **Settings → System integration**. Each row shows what is set up and what is
not. Rows marked with a lock change system files; clicking *Set up…* first shows
you the **exact commands**, then your desktop asks for your password. Nothing runs
if you cancel.

### SteelSeries headset access
Reading battery level and writing the onboard equalizer of a SteelSeries headset
means talking to `/dev/hidraw*`, which is root-only by default. The fix installs a
udev rule granting the logged-in user access to SteelSeries devices only:

```sh
install -m 0644 70-linuxsonar-steelseries.rules /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=hidraw
```

Undo it by deleting `/etc/udev/rules.d/70-linuxsonar-steelseries.rules`.

### Bluetooth battery reporting
BlueZ only exposes battery level with its experimental interfaces enabled. The fix
adds a systemd drop-in (it does **not** edit the distribution's own unit or
`main.conf`) and restarts the Bluetooth service — connected devices drop for a
moment:

```sh
/etc/systemd/system/bluetooth.service.d/10-linuxsonar.conf
```

Undo it by deleting that file and running `systemctl daemon-reload && systemctl
restart bluetooth`.

## Start automatically at login

**Settings → Background → "Start LinuxSonar when I log in"** writes
`~/.config/autostart/LinuxSonar.desktop`, which launches the app hidden
(`--background`) so its audio routing is live without a window in your face.

Note that closing the window keeps LinuxSonar running in the background — its
virtual channels disappear from other apps if the process exits. Use
**Settings → Background → Quit LinuxSonar** (or the tray icon, where your desktop
has one) to exit fully. Launching the AppImage again brings the window back
instead of starting a second copy.

> **GNOME note:** GNOME has no system tray by default, so no tray icon appears.
> Everything else works; reopen the window by launching LinuxSonar again.

## Where settings live

```
~/.config/LinuxSonar/LinuxSonar/settings.json   mixer, EQ, routing, preferences
~/.config/LinuxSonar/LinuxSonar/presets/        saved equalizer presets
```

Delete that directory to reset LinuxSonar to defaults.
