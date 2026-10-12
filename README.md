# T-Dongle-S3 Wi-Fi and Bluetooth survey

Firmware for the LilyGO T-Dongle-S3 (ESP32-S3) that turns the dongle into a
pocket radio survey tool. It scans continuously and shows every Wi-Fi access
point, or every Bluetooth Low Energy device, it can hear, with signal strength,
so you can walk a building and see where coverage falls off and whether you
need more APs or beacons. A double press of the button switches between the
two radios.

## Installing it on the dongle

### What you need

- A LilyGO T-Dongle-S3. It plugs straight into a USB-A port; use a USB-A
  extension or hub if your machine only has USB-C.
- PlatformIO Core. On macOS: `brew install platformio`. Elsewhere:
  `pip install platformio`. If you use VS Code, the PlatformIO IDE extension
  installs Core for you and adds Build / Upload / Monitor buttons to the
  status bar.
- No drivers. The ESP32-S3's native USB port shows up as a serial device on
  macOS, Linux and Windows 10 or later without anything extra.

### Flash it

```sh
git clone git@github.com:cpierce/lilygo-t-dongle-s3-wifi-ble.git
cd lilygo-t-dongle-s3-wifi-ble
pio run -t upload
```

The first build downloads the ESP32 toolchain, the Arduino core, TFT_eSPI and
NimBLE-Arduino, which is a few hundred megabytes and takes several minutes.
Builds after that take seconds. PlatformIO finds the dongle's serial port on
its own; if more than one serial device is plugged in, name it:

```sh
pio run -t upload --upload-port /dev/cu.usbmodem101   # macOS
pio run -t upload --upload-port /dev/ttyACM0          # Linux
pio run -t upload --upload-port COM5                  # Windows
```

On Linux your user needs access to the serial device: add yourself to the
`dialout` group (`uucp` on Arch), then log out and back in.

When the upload finishes the dongle resets, shows the splash screen for a
moment, and the Wi-Fi list fills in within a couple of seconds. Nothing else
needs configuring.

### Watch the log and run checks

```sh
pio device monitor         # JSON log of every scan, 115200 baud (Ctrl-C to stop)
pio test -e native         # unit tests, run on this machine (see Testing)
pio check                  # static analysis (cppcheck) of src/ and lib/
```

Opening the folder in VS Code with the PlatformIO extension gives IntelliSense
for the Arduino and ESP-IDF headers. If it complains about missing includes,
run `pio project init --ide vscode` to regenerate its configuration.

### If the upload fails

If the serial upload fails with "Failed to connect ... No serial data received",
the chip is usually stuck in the ROM bootloader. Unplug the dongle, plug it back
in, and run the upload again. If it still will not connect, hold the button
while plugging it in, upload, then unplug and replug. As a last resort the
`t-dongle-s3-jtag` environment flashes over the S3's built-in USB JTAG and
does not need the serial bootloader at all (it needs OpenOCD, which
PlatformIO downloads on first use):

```sh
pio run -e t-dongle-s3-jtag -t upload
```

If the picture is upside down for the way you hold it, change
`-DSCREEN_ROTATION=1` to `3` in `platformio.ini` and reflash.

## Testing

The code is split so that most of it can be tested without a dongle:

- `lib/survey/` holds everything that does not need a radio or a screen: the
  AP and BLE tables (smoothing, sorting, forgetting, recycling when full),
  advertisement decoding (maker IDs, iBeacon, Eddystone, service UUIDs), the
  button gesture decoder, the view/highlight navigation, and the JSON log
  line formatting. It has no Arduino dependency.
- `src/main.cpp` is the glue: Wi-Fi and NimBLE drivers, the display, the LED
  and the serial port.

**Unit tests** live in `test/` and run on your machine with PlatformIO's
Unity runner. They take a couple of seconds:

```sh
pio test -e native
```

They cover the colour bands in the legend above, the 3:1 RSSI smoothing
(including that it settles on the true level when a signal fades), the
three-missed-sweeps and ten-second forgetting rules, what happens when a
table is full, the exact JSON each record produces, a real iBeacon payload,
and every button gesture including hold auto-repeat and `millis()` rollover.

**Smoke test on the dongle.** With a flashed dongle plugged in:

```sh
python3 hil/check_dongle.py
```

It listens to the log for a few seconds on each radio, drives the firmware
through the same serial letters the button gestures map to, and checks that
every line is valid JSON with the documented fields, that press and hold move
through the views in the documented order, that both radios produce sweeps,
and that the firmware never resets. It needs `pyserial`; the Python that
ships with PlatformIO already has it.

**CI.** `.github/workflows/ci.yml` runs the unit tests, builds the firmware
and runs `pio check` on every push and pull request.

## Using it

The one button on the dongle does everything. It boots scanning Wi-Fi.

| Action | List view | Details view | Track view | Channel view |
|---|---|---|---|---|
| Press | Open details of the highlighted item | Track it | Wi-Fi: channel view. BLE: back to list | Back to list |
| Double press | Switch radio: Wi-Fi <-> BLE | Switch radio | Switch radio | Switch radio |
| Hold (0.6 s) | Move highlight down (wraps, pages) | Next item | Next item | nothing |

Keep holding and the highlight keeps stepping, about three rows a second, so
a long list is a single hold rather than a run of presses. A single press is
acted on about a third of a second after release, which is how long the
firmware waits to be sure a second press is not coming.

Switching radios clears the list so the new radio starts from a clean sweep;
the first results appear within a couple of seconds.

### Wi-Fi

**List view** shows every AP sorted strongest first. Each row has a 4-bar
meter, the SSID, the channel, and the smoothed RSSI in dBm. Rows go grey when
an AP was missed on the latest sweep and drop off after three misses. Hidden
networks show as `<hidden>`.

**Details view** is everything the scan knows about one AP: BSSID, channel
and width (20 MHz, or 40 MHz with the second half above or below), security
mode and cipher, the 802.11 modes it advertises (b/g/n) and whether WPS is on,
the raw and smoothed RSSI, how many sweeps have heard it, and how many other
APs broadcast the same SSID with the strongest one's RSSI and channel. A 40 MHz
AP on 2.4 GHz or a lone WPS-enabled AP is worth noticing on a survey.

**Track view** follows one SSID. The big number is the strongest AP of that
network with its channel and the last three bytes of its MAC, a gauge with a
white tick at -67 dBm, and up to three more APs broadcasting the same SSID
below it. This is the view to use while walking: when the number goes orange
or red, you are in a spot that needs another AP (or one moved closer).

**Channel view** is a bar per 2.4 GHz channel showing how many APs sit on it.
Bars are coloured by the loudest AP on that channel, and 1, 6 and 11 are
highlighted. Use it to pick channels for new APs.

### Bluetooth

**List view** shows every BLE device that is advertising, sorted strongest
first: a 4-bar meter, the advertised name (or the last three bytes of the
address when there is no name), a maker tag, and the smoothed RSSI. The tag
comes from the manufacturer data in the advertisement: `Apple`, `MSFT`,
`Googl`, `Samsg`, `Garmn`, `Sonos` and so on, `iBcn` for an iBeacon, `Eddy`
for an Eddystone beacon, blank when the maker is unknown. Rows go grey after
two seconds without an advertisement and drop off after ten.

**Details view** is everything the advertisements have said about one device:
full address and whether it is public or random, the maker tag and Bluetooth
SIG company ID, raw and smoothed RSSI, advertisement count, rate and time since
the last one, the 16-bit service UUIDs it advertises (or iBeacon major/minor
for an iBeacon), its advertised TX power, whether it is connectable, and the
manufacturer data length. Details fill in over time as different advertisement
types arrive.

**Track view** follows one device by address. It shows the RSSI, the maker
tag and whether the address is public (`pub`) or random (`rnd`), the full
address, a gauge with a white tick at -75 dBm, how long ago it was last heard,
and how many advertisements it has sent and at what rate. Use it to place
beacons or to walk toward a device.

The RGB LED mirrors the colour of the highlighted item (list) or the item
being looked at (details, track), so you can glance at it instead of reading
the screen.

## Colour legend

| Colour | Wi-Fi RSSI | BLE RSSI | Meaning |
|---|---|---|---|
| Green | -60 dBm or better | -65 or better | Excellent. |
| Yellow | -61 to -67 | -66 to -75 | Good. The usual design floor (voice/video for Wi-Fi, reliable positioning for beacons). |
| Orange | -68 to -75 | -76 to -85 | Fair. Works, with dropouts. |
| Red | below -75 | below -85 | Poor. Clients roam, drop, or crawl. |

BLE bands sit about 10 dB lower because a phone or beacon transmits far less
power than an access point.

## Serial log

Every sweep writes one summary line, then one JSON line per device, to the USB
serial port. `ms` is milliseconds since boot.

Wi-Fi: `rssi` is the raw reading from that sweep, `took` is how long the sweep
ran, `bw` is the channel width in MHz, `phy` the 802.11 modes advertised, and
`cipher` the pairwise cipher. A full sweep of all 13 channels takes about
1.7 s. Hidden networks log with an empty `ssid`.

```json
{"event":"sweep","scan":12,"ms":34567,"found":23,"took":1711,"status":0}
{"scan":12,"ms":34567,"ssid":"Office","bssid":"A4:2B:B0:11:22:33","ch":6,"rssi":-61,"auth":"WPA2","bw":20,"phy":"11b/g/n","cipher":"CCMP","wps":false}
```

Bluetooth: the table is dumped every 2 s. `rssi` is the last advertisement
heard, `age` is milliseconds since it, `adv` is the number of advertisements
heard since the device first appeared, `type` is `pub` or `rnd`, `conn` is
whether it is connectable, and `dropped` counts advertisements the firmware
could not keep up with (normally 0). `mfr` (company ID), `major`/`minor`
(iBeacon), `tx` (advertised TX power) and `svc` (16-bit service UUIDs) appear
only when the device has sent them.

```json
{"event":"radio","ms":40120,"radio":"ble"}
{"event":"ble_sweep","scan":3,"ms":46120,"found":17,"dropped":0}
{"scan":3,"ms":46120,"name":"AirPods Pro","addr":"5C:1B:F4:AA:BB:CC","type":"rnd","rssi":-58,"tag":"Apple","age":312,"adv":41,"conn":true,"mfr":"004C","tx":12,"svc":["FE24"]}
```

A `view` event is written whenever the button changes the screen, with the
view name, the highlight position and the BSSID or address of the item under
it, so a walk log shows which network or device was being tracked when:

```json
{"event":"view","ms":52010,"view":"track","cursor":2,"key":"A4:2B:B0:11:22:33"}
```

Rare `scan_timeout`, `scan_start_failed` or `ble_restart` events mean the
radio driver hiccupped; the next sweep starts on its own.

The dongle can also be driven from the laptop over the serial port: `w`
(Wi-Fi), `b` (BLE) or `r` (toggle) switch the radio, `n` steps to the next
item like a hold, and `v` cycles the view like a press.

Capture it on a laptop while walking to build a coverage record:

```sh
pio device monitor --quiet | tee survey.jsonl
```

## Limits

- **2.4 GHz only.** The ESP32-S3 radio has no 5 GHz or 6 GHz. If your APs run
  both bands, this shows the 2.4 GHz footprint, which is always the larger of
  the two. Anywhere the 2.4 GHz signal is weak, the 5 GHz signal is weaker.
- **BLE only.** The ESP32-S3 has no Bluetooth Classic, so headsets and
  speakers that only speak Classic are invisible. Phones and laptops rotate
  their random addresses every 15 minutes or so, so one phone can show up as
  a new device after a while.
- RSSI is as heard by this tiny antenna. A phone or laptop will read a few dB
  different, but the relative picture across the building is what matters.
- The list holds 48 APs or 48 BLE devices. In a dense office the weakest ones
  recycle.

## Hardware notes

Pins (from LilyGO's repo): LCD on SPI with MOSI 3, SCLK 5, CS 4, DC 2, RST 1,
backlight 38 (active low). Button on GPIO 0. APA102 LED data 40, clock 39.
Display is an 80x160 ST7735 driven with TFT_eSPI's `ST7735_GREENTAB160x80`
profile, which sets the 26/1 pixel offsets, BGR order and inversion.

TFT_eSPI needs `USE_HSPI_PORT` on the ESP32-S3 with Arduino core 2.0.x. Without
it the library's default port maps its SPI registers to address zero and the
board crash-loops with a `StoreProhibited` panic inside `TFT_eSPI::init`.

Bluetooth uses NimBLE-Arduino. Both radio stacks stay initialised; switching
only starts and stops the scans, so there is nothing to re-initialise and the
switch is instant. The Wi-Fi driver and BLE host share the one 2.4 GHz radio,
which is why only one of them scans at a time.

## License

MIT, see `LICENSE`. The libraries PlatformIO pulls in at build time keep their
own licences: TFT_eSPI (FreeBSD), NimBLE-Arduino (Apache 2.0) and the
Arduino-ESP32 core (LGPL 2.1). None of them is vendored into this repo.
