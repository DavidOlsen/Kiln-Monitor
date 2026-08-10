# ESP32 Kiln Monitor

A passive kiln-uniformity tester. Built on the same ESP32 + touchscreen hardware as [ElectricKiln](https://github.com/pllagunos/ElectricKiln), but instead of controlling a kiln, this device just watches it: up to three MCP9600 thermocouple probes (typically placed in the upper, middle, and lower sections of a kiln) report their temperatures, and the device automatically logs them to InfluxDB for the duration of a firing so you can check whether the whole kiln is heating evenly.

It has no heater outputs and does no temperature control — it's purely an instrument.

## How it works

* **1 to 3 zones, auto-detected.** Plug in one, two, or three MCP9600 probes at I2C addresses `0x67`/`0x66`/`0x65`; whichever respond on the bus become active zones. No configuration needed to change probe count.
* **Auto start:** at boot (and after each test ends), the device captures an "ambient" baseline — the average of all active zones at rest. Once *any* zone rises more than `AMBIENT_START_DELTA` (15°C by default) above that baseline, logging to InfluxDB begins automatically.
* **Auto stop:** logging continues until *every* active zone has returned within `AMBIENT_STOP_DELTA` (5°C by default) of the ambient baseline — conservative, so it won't stop early while any section is still hot. Ambient is then re-captured, so the device is ready for a repeat test without a reboot.
* **Status screen** shows live per-zone temperatures and whether a test is currently being logged. **Config screen** sets thermocouple type (K or S) and WiFi.

Thresholds are compile-time constants in `src/EKcommon.h` (`AMBIENT_START_DELTA`, `AMBIENT_STOP_DELTA`) — adjust and reflash if 15°/5° doesn't suit your kiln.

## Hardware

Same board/wiring pattern as ElectricKiln — see [`hardware/`](hardware/) for the shared PCB design. Differences from ElectricKiln's build:

* Thermocouple interface: **MCP9600 over I2C** (SDA=IO32, SCL=IO25) instead of a single MAX31856 over SPI.
* **No relay/heater outputs** — this device doesn't drive anything.

## Software and Installation

Built with [PlatformIO](https://platformio.org/).

### Dependencies

Installed automatically by PlatformIO — see `platformio.ini`:

* `moononournation/GFX Library for Arduino` — display driver
* `paulstoffregen/XPT2046_Touchscreen` — touch driver
* `adafruit/Adafruit BusIO`, `adafruit/Adafruit MCP9600 Library` — thermocouple driver
* `tobiasschuerg/ESP8266 Influxdb` — InfluxDB client
* `me-no-dev/AsyncTCP`, `me-no-dev/ESPAsyncWebServer` — captive portal / config web server
* `bblanchon/ArduinoJson`

### Installation Steps

1. **Upload the filesystem image first.** The web UI (`/data`) must be on the device before first use — in PlatformIO, run **"Upload Filesystem Image"**.
2. **Build and upload firmware** — PlatformIO **"Upload"**, or `pio run --target upload`.

## Usage

The touchscreen has two screens:

* **Status** (home screen): per-zone temperatures, a "LOGGING ACTIVE" / "WAITING" indicator, ambient baseline, and current TC type. Tap **SETTINGS** to configure.
* **Config**: tap **TC TYPE** to cycle between K and S (shared by all zones), **TEMP** to toggle °C/°F, **RESET WIFI** / **EXIT WIFI** for WiFi setup, **DONE** to go back.

A bottom error bar reports thermocouple faults per zone (e.g. `TC FAULT Z2: Input range fault`), or `NO THERMOCOUPLES DETECTED` if the I2C scan found nothing.

### First Boot — WiFi Setup

1. On the touchscreen: **SETTINGS → RESET WIFI**.
2. Connect your phone/computer to the WiFi network **"ESP32 Kiln Monitor"**.
3. A captive portal should open automatically; if not, go to `http://192.168.4.1`.
4. **WiFi Manager** → select your network → enter password → Submit.
5. Once connected, tap **EXIT WIFI** on the device.

Once on your network, the device is also reachable at `http://kilnmonitor.local` (mDNS) where supported.

### InfluxDB Setup

Configured entirely via the web UI, no recompile needed — same flow as ElectricKiln:

1. Open the device's IP (or the captive portal) → **InfluxDB Manager**.
2. Fill in URL, API token, org, bucket, and a POSIX timezone string.
3. Save. Credentials persist in LittleFS across reboots. The token is never sent back to the browser on subsequent visits.

Published fields per active zone: `Zone<N> temperature`, `Zone<N> fault`, plus `Ambient`. Only written while a test is actively logging (see "How it works" above) — there's no continuous idle logging.

### OTA Firmware Updates

Same GitHub-Releases-based mechanism as ElectricKiln (see `src/ota/ota.cpp`). Points at [DavidOlsen/Kiln-Monitor](https://github.com/DavidOlsen/Kiln-Monitor), which must be **public** — the device makes unauthenticated GitHub API calls. Cut a GitHub release to trigger `.github/workflows/build-release.yml`, which builds against the `e32r40t_gfx` PlatformIO environment and attaches the firmware/filesystem images plus their MD5 checksums to the release.

## License

Same dual-license model as ElectricKiln:

- Firmware/software: **MIT License** — see [`LICENSE`](LICENSE).
- Hardware design files in [`hardware/`](hardware/): **CERN-OHL-W v2** — see [`LICENSE-HARDWARE`](LICENSE-HARDWARE).
