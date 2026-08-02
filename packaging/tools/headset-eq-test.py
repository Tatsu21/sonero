#!/usr/bin/env python3
"""Reverse-engineering helper for the SteelSeries Arctis Nova Pro Wireless
onboard EQ. YOU run this on YOUR device. It only touches the EQ (reversible).

Encoding hypothesis: 10 bands, byte = 0x14 + round(gain_dB * 2), clamped 0x00..0x28
(so 0x14 = 0 dB, 0x28 = +10 dB, 0x00 = -10 dB).

Usage (have music playing, then listen):
  python3 headset-eq-test.py battery
  python3 headset-eq-test.py flat
  python3 headset-eq-test.py lowboost     # +6 dB on the 3 LOWEST bands only
  python3 headset-eq-test.py highboost    # +6 dB on the 3 HIGHEST bands only
  python3 headset-eq-test.py sonar        # a tasteful gaming curve
  python3 headset-eq-test.py db  3 3 2 -1 0 1 3 3 2 1   # 10 gains in dB
  # add  --nosave  to any command to skip the 0x09 commit
"""
import os, select, sys, glob, time

EQ_CMD = 0x33
CENTER = 0x14   # 0 dB
STEP = 2        # bytes per dB

def find_dev():
    for h in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            ue = open(h + "/device/uevent").read()
            rd = open(h + "/device/report_descriptor", "rb").read()
        except OSError:
            continue
        if ("00001038:000012E0" in ue.upper()) and (b"\x06\xc0\xff" in rd):
            return "/dev/" + os.path.basename(h)
    return "/dev/hidraw6"

def enc(gain_db):
    return max(0x00, min(0x28, round(CENTER + gain_db * STEP)))

def send(fd, payload, label):
    os.write(fd, bytes(payload) + bytes(64 - len(payload)))
    print(f"sent [{label}]: " + " ".join(f"{b:02x}" for b in payload))

def read_status(fd):
    # The sequence that actually changed the sound read the status right before
    # sending the EQ. Replicate that handshake (request + drain the reply).
    os.write(fd, bytes([0x06, 0xb0]) + bytes(62))
    select.select([fd], [], [], 0.3)
    try:
        os.read(fd, 64)
    except OSError:
        pass

def set_eq(fd, gains_db, label, save=True):
    assert len(gains_db) == 10
    read_status(fd)
    bands = [enc(g) for g in gains_db]
    send(fd, [0x06, EQ_CMD] + bands, f"{label}  dB={gains_db}")
    if save:
        time.sleep(0.2)  # let the device apply before committing
        send(fd, [0x06, 0x09], "save")

def main():
    args = [a for a in sys.argv[1:] if a != "--nosave"]
    save = "--nosave" not in sys.argv
    if not args:
        print(__doc__); return
    fd = os.open(find_dev(), os.O_RDWR)
    mode = args[0]
    if mode == "battery":
        os.write(fd, bytes([0x06, 0xb0]) + bytes(62)); select.select([fd], [], [], 0.5)
        r = os.read(fd, 64)
        print(f"battery: headset {r[6]*10}%  spare {r[11]*10}%")
    elif mode == "flat":
        set_eq(fd, [0] * 10, "flat", save)
    elif mode == "lowboost":
        set_eq(fd, [6, 6, 6, 0, 0, 0, 0, 0, 0, 0], "lowboost", save)
    elif mode == "highboost":
        set_eq(fd, [0, 0, 0, 0, 0, 0, 0, 6, 6, 6], "highboost", save)
    elif mode == "extreme":  # the profile that audibly changed the sound before
        set_eq(fd, [10, 10, 8, 4, 0, 0, -6, -8, -10, -10], "extreme", save)
    elif mode == "sonar":
        set_eq(fd, [3, 3, 2, -1, 0, 1, 3, 3, 2, 1], "sonar", save)
    elif mode == "db":
        set_eq(fd, [float(x) for x in args[1:11]], "db", save)
    else:
        print("unknown mode")
    os.close(fd)

if __name__ == "__main__":
    main()
