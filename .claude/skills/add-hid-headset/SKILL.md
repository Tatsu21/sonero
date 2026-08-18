---
name: add-hid-headset
description: Add battery-level reporting for a new USB HID headset to Sonero. Use when the ask is about a headset's battery percentage rather than about audio routing. Battery only — the onboard equalizer is explicitly out of scope.
---

The full procedure lives in `hid/SKILL.md`, next to the code it describes. Read that
file and follow it in order — it covers identifying the device, the udev access rule,
where to source the protocol from, and how to verify the result on real hardware.

Three rules from it worth loading before you start:

- **Battery only.** Do not attempt the onboard equalizer, sidetone or ANC. The EQ code
  that exists writes successfully to the one supported headset and changes nothing
  audible; there is no working example to copy.
- **Do not edit `hid/SteelSeriesDevice.{h,cpp}`.** Write new files for the new device and
  use that one as a reference. It is the only HID path proven on real hardware, and no
  test would catch you breaking it.
- **A successful `write()` proves nothing**, and never invent report offsets — cite an
  observed report or HeadsetControl.
