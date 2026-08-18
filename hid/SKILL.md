---
name: add-hid-headset
description: Add battery-level reporting for a new USB HID headset to Sonero by writing a new device class alongside the existing one. Use when someone wants a battery percentage for a headset that is not the SteelSeries Arctis Nova Pro Wireless.
---

# Adding a HID headset to Sonero

A procedure for an AI assistant to follow end to end, so the person asking never has to
read the C++ themselves. Follow it in order. It is written from the mistakes actually
made while adding the first device — the ground rules below are not decoration.

## Scope: battery level, and nothing else

Sonero needs **no HID at all to play audio**. Routing, volume, the equalizer and the
mixer work with any headset the moment PipeWire sees it. HID is only for the sidechannel
a headset exposes over USB.

Of that sidechannel, this procedure covers **battery level only**.

**Do not attempt the onboard equalizer.** Sonero contains code that writes an EQ to the
Arctis Nova Pro Wireless; the writes are accepted by the device and produce no audible
change, and nobody has worked out why. Since it has never been made to work on the one
model that was in hand, there is no working example to copy and no way to tell a correct
implementation from a broken one. The same applies to sidetone, ANC modes and any other
setting the headset stores internally. If a user asks for those, tell them plainly that
it is unsolved rather than shipping code that appears to succeed.

Bluetooth battery is a different path entirely (BlueZ `org.bluez.Battery1`, see
`btBatteryPercent()` in `ui/DevicesPage.cpp`) and needs nothing from this file. This is
about **wired USB HID**.

## Ground rules

1. **Never invent report offsets.** Every constant must come from a report you observed
   changing, or from a published implementation. The first version of this code read
   bytes 10 and 11 and looked right for a while purely by coincidence; the correct
   offsets (6 and 15) came from [HeadsetControl](https://github.com/Sapd/HeadsetControl),
   `src/devices/*.c`. If you cannot point at where a number came from, you do not have
   that number yet.
2. **A successful `write()` proves nothing.** `writeReport()` returning true means the
   kernel accepted the bytes, not that the device did anything with them. That is
   exactly how the onboard EQ ended up looking finished while doing nothing. A battery
   readout is only real when the number *moves* with the actual charge.
3. **You cannot observe hardware yourself.** Ask the user to power the headset off, put
   it on the dock, unplug the cable — one change at a time — and paste what the tool
   printed. Do not guess what the hardware did.
4. **Report what you could not verify.** If the user cannot test something, say so in
   the summary and in the README instead of implying it works.

## Step 1 — identify the device

Ask the user to run these and paste the output.

```sh
lsusb                                   # find the vendor:product id, e.g. 1038:12e0
for d in /sys/class/hidraw/hidraw*; do
  printf '%s: ' "$d"; grep HID_ "$d/device/uevent" | tr '\n' ' '; echo
done
```

`HID_ID=0003:00001038:000012E0` reads as `bus:VID:PID`, hex, zero-padded to 8 digits.
A headset usually exposes **several** hidraw nodes: the audio-control one (volume keys)
and a vendor one. The vendor node is the one you want.

Tell them apart by their report descriptor — the vendor collection opens with a
vendor-defined usage page:

```sh
xxd /sys/class/hidraw/hidrawN/device/report_descriptor | head -3
```

Bytes `06 c0 ff` mean *Usage Page (Vendor Defined 0xFFC0)*, which is what the
SteelSeries code looks for. Another vendor will use `06 XX ff` with a different `XX`, or
may need a different discriminator entirely. Note what you find.

## Step 2 — get permission to open it

`/dev/hidraw*` is root-only by default. The shipped rule
(`packaging/udev/70-sonero-steelseries.rules`) grants access to vendor `1038` only:

```
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1038", TAG+="uaccess", MODE="0660", GROUP="plugdev"
```

For a different vendor, add a line with that `idVendor`. Keep `uaccess` — it grants
access to the logged-in user only, which is narrower than a group. Then:

```sh
sudo cp packaging/udev/70-sonero-steelseries.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
```

Replug the device afterwards: udev applies rules when a device appears, so a running
device keeps its old permissions.

## Step 3 — find the protocol before writing code

In order of preference:

1. **[HeadsetControl](https://github.com/Sapd/HeadsetControl)** — `src/devices/` has one
   file per model with the request bytes, the response offsets and the battery scale,
   and `Devices.md` lists what is supported. This is where the Nova Pro's decode came
   from. Read the file for the exact model: vendors change offsets between products.
2. **The device's own report descriptor**, for report ids and lengths.
3. **Observation, last resort.** Send the candidate request, dump the reply, and have
   the user change one thing at a time. A byte that moves in step with charge on a small
   scale (0..4, 0..8, 0..100) is your battery; a byte that flips between a few fixed
   values is your link state. `hid/probe_main.cpp` has a `--watch` loop for the
   SteelSeries device that prints a line only when the report changes and brackets the
   bytes that moved — copy that idea for the new device.

Write down the request bytes, the response offsets, the scale, and **the source**. The
source goes in a comment next to the constants.

## Step 4 — new files, and do not touch the working device

**`hid/SteelSeriesDevice.h` and `hid/SteelSeriesDevice.cpp` are off limits.** They are
the one HID path known to work on real hardware, and there is no test that would catch
you breaking them. Read `SteelSeriesDevice.cpp` as the worked example — it is short and
complete — then write a **new, standalone pair of files** for the new device:

```
hid/<Vendor><Model>Device.h     e.g. hid/CorsairVoidDevice.h
hid/<Vendor><Model>Device.cpp
```

Copy the *shape*, not the constants:

- the same public surface — `probe()`, `open()`, `close()`, `isOpen()`, `readBattery()`,
  and the low-level `writeReport()` / `readReport()` pair;
- the same discovery approach — walk `/sys/class/hidraw`, match `HID_ID` against your
  VID:PID, then confirm the interface with something from the report descriptor, and
  return `/dev/hidrawN`;
- the same permission-free `probe()` that reads `/sys` and only tries `::open()` to
  report whether access is granted;
- the same result structs (`ProbeResult`, `BatteryStatus`, `HeadsetState`) — reuse them
  from `SteelSeriesDevice.h` by including it, or declare equivalents in your own header
  if the model reports something the existing structs cannot express. Reusing is fine;
  editing them is not.

Do not add an abstract base class over the two devices, and do not "refactor while you
are in there". A second independent file that duplicates thirty lines of `/sys` walking
is the correct trade here: it cannot regress the device that works.

Then wire it in — these files you *may* edit:

| File | Change |
|---|---|
| `CMakeLists.txt` | add the two new files to the `sonero_core` source list |
| `packaging/udev/70-sonero-steelseries.rules` | a line for the new `idVendor` |
| `hid/probe_main.cpp` | probe and dump the new device too, so there is a CLI way to test it |
| `ui/DevicesPage.cpp` | a card for the new device, following `buildSteelSeriesControls()` for the widgets and `DevicesPage::refresh()` for the polling |

Match the surrounding style: comments explain *why* a constant is what it is and cite
the source, `[[nodiscard]]` on queries, no exceptions, and no new dependencies — this
code talks to `/dev/hidraw` directly and must stay that way.

## Step 5 — verify, then say exactly what you verified

```sh
cmake --build build --parallel && ctest --test-dir build --output-on-failure
./build/hidprobe            # present, accessible, and a plausible percentage
```

Then run Sonero and look at the Devices page. Ask the user to:

- compare the number with whatever the headset itself reports (its own app, a voice
  prompt, the dock display);
- let the headset discharge, or take it off the charger, and confirm the number follows.

A percentage that never changes is not a working battery readout — it is a constant that
happens to look reasonable. That is the failure mode to rule out before claiming
success.

## Step 6 — document what you added

- `README.md`, the **Tested hardware** table: add the model **only if it was confirmed
  on real hardware**, with the connection type. If it could not be tested, say so
  explicitly instead of listing it.
- `docs/INSTALL.md`: if you added a new vendor id to the udev rule, mention it in the
  headset-access section, which then no longer covers only SteelSeries.
- The commit message should carry the VID:PID and where the protocol came from.

## Hand back

A short report with: the device's VID:PID and control path, where each protocol constant
came from (link or observed report), what you verified and how, and what remains
unverified. The last item is the important one — this project would rather ship
"battery works, nothing else attempted" than a confident claim that turns out to be
false.
