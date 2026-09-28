# Quiesco firmware

The Arduino C++ firmware for the Quiesco unit: a Seeed XIAO nRF52840 Plus that
measures CO₂, temperature, humidity, pressure, light and noise, logs them to
flash, draws them on a 1.54″ e-ink panel and serves them to the companion app
over Bluetooth Low Energy.

It is a single cooperative state machine with no application threads, every
wait bounded by a deadline, and one adapter file per chip, so a sensor, the
display or the BLE stack can be swapped by touching one file.

> [!NOTE]
> Version **0.1.0** · BLE protocol **v6** · board core **Seeeduino mbed 2.9.3**
> (`Seeeduino:mbed:xiaonRF52840Plus`)

## Contents

- [Quick start](#quick-start)
- [Test a new unit](#test-a-new-unit)
- [Commands](#commands)
- [Debugging](#debugging)
- [Source layout](#source-layout)
- [Documentation](#documentation)
- [Licence](#licence)

## Quick start

### Prerequisites

- macOS or Linux with `make`, `bash` and `python3`
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) on your `PATH`, or the
  Arduino IDE 2 installed in `/Applications` (the scripts use its bundled CLI)
- For the host tests only: `brew install mbedtls@3`

### Build and flash

```sh
cd firmware

# 1. Install the pinned board core and libraries (once)
scripts/install-dependencies.sh

# 2. Build the release image into .build/quiesco_firmware/
make build

# 3. Plug the unit in over USB-C and upload
scripts/arduino-cli.sh upload --fqbn Seeeduino:mbed:xiaonRF52840Plus \
  -p /dev/cu.usbmodem101 --input-dir .build/quiesco_firmware .
```

Your serial port may differ: `scripts/arduino-cli.sh board list` shows it.
Within about 15 seconds the panel draws its first reading.

> [!TIP]
> Uploading rewrites only the MCU's internal flash. The external flash (config,
> paired phones and the sample log) survives, so you can swap between release
> and debug builds without losing data.

### Using the Arduino IDE instead

Install the **Seeed nRF52 mbed-enabled Boards** package at version **2.9.3**
and select **XIAO nRF52840 (Sense) Plus**. Install the libraries at the exact
versions in [`dependencies.lock`](dependencies.lock), then open `firmware.ino`.
The `make` targets remain the reference build: they also stamp the version and
set the build flags.

## Test a new unit

The `smoke/sensors` sketch is the factory test. It exercises every part through
the production drivers and prints a PASS/FAIL line for each.

```sh
make smoke
scripts/arduino-cli.sh upload --fqbn Seeeduino:mbed:xiaonRF52840Plus \
  -p /dev/cu.usbmodem101 --input-dir .build/smoke-sensors smoke/sensors
```

Open the serial port at **115200** baud and press **`t`**:

```text
unit test:
  BME280   PASS
  VEML7700 PASS
  SCD41    PASS
  mic      PASS
  battery  PASS
  flash    PASS
  display  PASS
  BLE      PASS
RESULT: PASS
```

Then check the panel shows a complete test pattern, and find
`Quiesco TEST XXXX` in a BLE scanner such as nRF Connect. When everything
passes, flash the release firmware as in [Quick start](#build-and-flash).

On a failure, the sketch prints a hint. Its other single-key commands (I²C
scan, rail settle sweep, live monitor, SCD41 probe and more) are described in
[`HARDWARE.md` §8](HARDWARE.md#8-unit-test-sketch--smokesensors), and the full
factory procedure in [§9](HARDWARE.md#9-factory-procedure).

## Commands

| Command | What it does |
|---|---|
| `scripts/install-dependencies.sh` | installs the pinned core and libraries from `dependencies.lock` |
| `make build` | release build (no serial output) into `.build/quiesco_firmware/` |
| `make build-debug` | adds a boot line and one status line per cycle on USB serial |
| `make build-trace` | debug build plus driver-level trace lines, also kept in a flash ring |
| `make test` | host unit tests in plain C++, no board needed |
| `make check` | source policy: SPDX headers, no raw pin numbers outside `BoardPins.h` |
| `make verify` | `check` + `test`; run it before opening a PR |
| `make smoke` | builds the test sketches `smoke/sensors` and `smoke/ble` |
| `make licences` | checks installed versions and gathers every licence into `.build/licences/` |

A unit built for the bench without a battery: `scripts/build.sh --no-battery`.

## Debugging

A debug build prints a status line per cycle that tells you at a glance which
part is at fault:

```text
cycle=1 T=28.36C RH=50.1% P=1011.6hPa CO2=384ppm lux=1.4 noise=51.6dB
battery=3.72V screen=bento draw=yes log=#42 bme=ok failures=0 veml=ok ...
```

It also accepts single-key commands over serial. `scripts/quiesco-console.py`
wraps them (Python standard library only):

| Command | What it does |
|---|---|
| `scripts/quiesco-console.py info` | device and config info |
| `scripts/quiesco-console.py dump log.csv` | saves the on-flash log as CSV |
| `scripts/quiesco-console.py monitor out.txt` | prints and saves every serial line, timestamped |
| `scripts/quiesco-console.py trace trace.txt` | reads back the trace ring of a trace build |
| `scripts/log-report.py log.csv` | lost records, resets, battery drain, sensor ranges; add `--reference ref.csv` to get calibration offsets |

The debugging playbook, known-good values and the Arduino core traps that
have cost real debugging sessions are in [`HARDWARE.md`](HARDWARE.md#6-arduino-core-traps).

## Source layout

```text
firmware.ino          setup() and loop() only
src/
  App.*               composition root and state machine
  board/              BoardPins (every pin), PowerDomain (the sensor rail)
  platform/           clock, watchdog, device ID, reset reason, I²C recovery
  model/              Reading, Config, FaultStatus
  drivers/            one adapter per chip: SCD41, BME280, VEML7700, PDM mic,
                      battery, e-ink, flash, BLE
  dsp/                A-weighted acoustic metrics
  services/           sampler, scheduler, calibration, config store,
                      sample log, bonds
  protocol/           BLE wire formats and PROTOCOL.md
  storage/            flash partition map, trace ring
  ui/                 comfort evaluation, screen model, renderer, fonts
  diagnostics/        build config, firmware version, debug log, trace
  third_party/        vendored FlashDB 2.2.0
smoke/                hardware test sketches (sensors, BLE)
tests/host/           host unit tests and fakes
scripts/              build, test, policy, dependency, console and font tools
```

## Documentation

| Document | Covers |
|---|---|
| [`SOFTWARE.md`](SOFTWARE.md) | what the firmware does, architecture, every feature, coding rules, status |
| [`HARDWARE.md`](HARDWARE.md) | the v1 board: power, buses, pin map, devices, core traps, factory test |
| [`UI.md`](UI.md) | the e-ink screens, comfort bands and refresh policy |
| [`src/protocol/PROTOCOL.md`](src/protocol/PROTOCOL.md) | the BLE contract with the app, with golden vectors |
| [`WORKPLAN.md`](WORKPLAN.md) | milestones left before production and the session log |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | dependencies and their licences |

## Licence

GPL-3.0-only, see [`LICENSE`](LICENSE). Every source file carries its SPDX
identifier. Dependencies are pinned in [`dependencies.lock`](dependencies.lock)
and listed with their licences in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

<sub>← Back to the [Quiesco overview](../README.md)</sub>
