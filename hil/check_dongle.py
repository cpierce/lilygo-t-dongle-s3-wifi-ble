#!/usr/bin/env python3
"""Smoke test against a real dongle running this firmware.

Plug a flashed dongle in and run:

    python3 hil/check_dongle.py              # finds the port on its own
    python3 hil/check_dongle.py --port /dev/cu.usbmodem101

It listens to the serial log, drives the firmware with the same single-letter
commands the button gestures map to (v = press, n = hold, b/w = radio), and
checks that every line is valid JSON with the documented fields, that the
views change in the documented order, that both radios produce sweeps, and
that the firmware never resets. Exit status 0 means all checks passed.

Needs pyserial (pip install pyserial); PlatformIO's own Python has it.
"""
import argparse
import glob
import json
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is needed: pip install pyserial")

WIFI_KEYS = {"scan", "ms", "ssid", "bssid", "ch", "rssi", "auth", "bw", "phy", "cipher", "wps"}
BLE_KEYS = {"scan", "ms", "name", "addr", "type", "rssi", "tag", "age", "adv", "conn"}
CRASH_MARKERS = ("Guru Meditation", "abort()", "Backtrace:", "rst:")


class Dongle:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.ser.dtr = False
        self.ser.rts = False
        self.lines = []       # every raw line, in order
        self.started = False  # first JSON line seen (boot banner is over)
        # Opening the port resets the ESP32-S3 (its USB-serial auto-reset
        # circuit sees the DTR/RTS change). Let that reset and the ROM boot
        # banner go by before anything is counted.
        self.pump(2.0)
        self.lines = []
        self.started = False

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                if not self.started and line.startswith("{"):
                    self.started = True
                self.lines.append((self.started, line))

    def send(self, c, settle=0.4):
        self.ser.write(c.encode())
        self.pump(settle)

    def mark(self):
        return len(self.lines)

    def events(self, since=0, name=None):
        out = []
        for started, line in self.lines[since:]:
            if not started or not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "event" in obj and (name is None or obj["event"] == name):
                out.append(obj)
        return out


class Checker:
    def __init__(self):
        self.failures = []
        self.passes = 0

    def check(self, ok, what):
        if ok:
            self.passes += 1
        else:
            self.failures.append(what)
        print(("  ok   " if ok else "  FAIL ") + what)


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="serial port (default: first USB modem found)")
    ap.add_argument("--settle", type=float, default=6.0, help="seconds to listen per radio")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("no dongle found; pass --port")
    print(f"dongle on {port}")
    d = Dongle(port)
    c = Checker()

    # --- Wi-Fi, the boot radio -------------------------------------------
    print("Wi-Fi")
    d.pump(args.settle)
    c.check(d.started, "serial log is flowing")
    banner = [l for started, l in d.lines if not started]
    c.check(not banner, f"no boot banner after the port settled ({len(banner)} lines)")
    sweeps = d.events(name="sweep")
    c.check(len(sweeps) >= 2, f"Wi-Fi sweeps arrive (got {len(sweeps)} in {args.settle:.0f} s)")
    c.check(all(s["status"] == 0 for s in sweeps), "every sweep reports status 0")

    bad, records = [], []
    for started, line in d.lines:
        if not started:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            bad.append(line)
            continue
        if "bssid" in obj:
            records.append(obj)
    c.check(not bad, f"every line is valid JSON ({len(bad)} bad)")
    c.check(len(records) > 0, f"AP records logged ({len(records)})")
    c.check(all(WIFI_KEYS <= set(r) for r in records), "AP records carry the documented fields")
    c.check(all(1 <= r["ch"] <= 14 and -100 <= r["rssi"] <= 0 for r in records), "AP channels and RSSI are plausible")
    c.check(all(r["bw"] in (20, 40) and r["phy"].startswith("11") for r in records), "AP bandwidth and PHY are well formed")

    # Press cycles the views; hold steps to the next item.
    print("Wi-Fi button gestures (v = press, n = hold)")
    m = d.mark()
    for cmd in "vnvvv":
        d.send(cmd)
    views = [e["view"] for e in d.events(m, "view")]
    c.check(views == ["details", "details", "track", "channels", "list"],
            f"press/hold sequence gives details, next, track, channels, list (got {views})")
    keys = [e.get("key") for e in d.events(m, "view")]
    c.check(len(keys) >= 2 and keys[0] != keys[1], "hold moved the highlight to a different AP")

    # --- Bluetooth ------------------------------------------------------
    print("Bluetooth")
    m = d.mark()
    d.send("b", settle=0.5)
    radio = d.events(m, "radio")
    c.check(radio and radio[-1]["radio"] == "ble", "radio switched to BLE")
    d.pump(args.settle)
    bsweeps = d.events(m, "ble_sweep")
    c.check(len(bsweeps) >= 2, f"BLE sweeps arrive (got {len(bsweeps)})")
    c.check(all(s["dropped"] == 0 for s in bsweeps), "no advertisements dropped")

    brecords, bad = [], []
    for started, line in d.lines[m:]:
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            bad.append(line)
            continue
        if "addr" in obj:
            brecords.append(obj)
    c.check(not bad, f"every BLE line is valid JSON ({len(bad)} bad)")
    c.check(len(brecords) > 0, f"BLE records logged ({len(brecords)})")
    c.check(all(BLE_KEYS <= set(r) for r in brecords), "BLE records carry the documented fields")
    c.check(all(r["type"] in ("pub", "rnd") and isinstance(r["conn"], bool) for r in brecords), "BLE type and conn are well formed")
    c.check(all(len(r.get("mfr", "0000")) == 4 for r in brecords), "mfr is a 4-digit company id when present")

    print("Bluetooth button gestures")
    m = d.mark()
    for cmd in "vnvv":
        d.send(cmd)
    views = [e["view"] for e in d.events(m, "view")]
    c.check(views == ["details", "details", "track", "list"],
            f"BLE press/hold sequence gives details, next, track, list (got {views})")

    # --- back to Wi-Fi, and nothing fell over -----------------------------
    m = d.mark()
    d.send("w", settle=0.5)
    radio = d.events(m, "radio")
    c.check(radio and radio[-1]["radio"] == "wifi", "radio switched back to Wi-Fi")
    d.pump(3)
    c.check(len(d.events(m, "sweep")) >= 1, "Wi-Fi sweeps resume")

    crashes = [l for started, l in d.lines if started and any(k in l for k in CRASH_MARKERS)]
    c.check(not crashes, f"no resets or panics during the run ({len(crashes)})")

    print(f"\n{c.passes} passed, {len(c.failures)} failed")
    return 0 if not c.failures else 1


if __name__ == "__main__":
    sys.exit(main())
