# Quiesco v1 — Firmware

This document describes what the Quiesco v1 firmware does, how it is
structured, how it was built, and what is still open. Hardware facts live in
[`HARDWARE.md`](HARDWARE.md); the display layouts in [`UI.md`](UI.md).

---

## 1. What it does

Every measurement interval (60, 300, 600 or 1800 s; default **300 s**) the
device:

1. switches on the peripheral rail and waits for it to settle;
2. measures temperature, humidity, pressure, CO2, light, noise and battery
   voltage, each with its own validity flag;
3. applies the user's calibration offsets;
4. appends the reading to an on-flash log;
5. redraws the e-ink panel if, and only if, the picture changed;
6. publishes the reading and status over BLE, and streams the log if a phone
   asked for it;
7. powers the rail down and sleeps until the next deadline.

Between cycles the MCU stays in system-on sleep with BLE available, so a phone
can change settings, sync the clock, download history or trigger a CO2
recalibration at any time. A missing or failing sensor never stops the others:
it is reported as unavailable, and the cycle continues.

---

## 2. Building, flashing and testing

| Command | What it does |
|---|---|
| `scripts/install-dependencies.sh` | installs the pinned core and libraries from `dependencies.lock` |
| `make build` | release build (no serial output) into `.build/quiesco_firmware/`, versioned from `VERSION` |
| `make build-debug` | the same with `QUIESCO_DEBUG=1`: one status line per cycle on USB serial |
| `make build-trace` | debug build plus `QUIESCO_TRACE=1`: driver-level lines for diagnosing a faulty unit (see below) |
| `make test` | host unit tests (plain C++, no board needed; needs `brew install mbedtls@3`) |
| `make check` | source policy: no raw pin numbers or banned macros outside `BoardPins.h` |
| `make verify` | `check` + `test` |
| `make smoke` | builds the hardware test sketches: `smoke/sensors` and `smoke/ble` |
| `make licences` | checks installed versions against `dependencies.lock` and gathers every licence text into `.build/licences/` (see `THIRD_PARTY_NOTICES.md`) |

Upload a build with:

```sh
scripts/arduino-cli.sh upload --fqbn Seeeduino:mbed:xiaonRF52840Plus \
  -p /dev/cu.usbmodem101 --input-dir .build/quiesco_firmware .
```

### Diagnosing a faulty unit

`make build-trace` adds one line per driver event to the debug output, in the
form `t=<ms> trace <source> <what> <value>`. Upload it as above and record it
with `scripts/quiesco-console.py monitor trace.txt`. What it shows:

| Source | Lines |
|---|---|
| `veml` | raw counts per range (`coarse`/`fine`/`bright counts`), `read retry`, I2C status on a failed transfer, `timeout`, final `lux` |
| `display` | BUSY wait after reset, `full`/`partial refresh ms`, `saw busy`, `busy still low` when a check fails |
| `bme280` | temperature, humidity, pressure as read; `missing`, `timeout` |
| `scd41` | Sensirion error codes (`probe`, `data ready`, `read`), `co2 ppm`, `single shot failed`, `timeout` |
| `mic` | `start timeout`, `capture timeout` with the buffers captured |
| `flash` | the JEDEC ID read when it is not `EF 40 17` |
| `i2c` | `sda held low` before recovery, `recovery failed` |

**Unattended units.** A trace build also keeps its history on the unit's
flash, in a 48 KB ring between the config and sample partitions
(`FlashStorageLayout::kTraceRing*`, `storage/TraceStore`). Each measurement
cycle appends a boot marker when there was one, the `trace` event lines, BLE
and other `DebugLog` events, and a compact cycle line
(`cycle=… lux10=… draw=… veml=o0 display=o0 …`: per device `o` ok, `m`
missing, `t` timeout, `e` read-error, then the failure count). That is about
180 bytes a cycle, so roughly the last 4.5 h at a 60 s interval; the sample
log keeps the readings for the whole period. Slow-step lines and float values
stay on serial only. Read the ring back, oldest first, with
`scripts/quiesco-console.py trace trace.txt` (console `t`).

The trace macros (`src/diagnostics/Trace.h`) compile to nothing unless
`QUIESCO_TRACE=1`, so release and plain debug images do not change. To add a
trace point, call `QUIESCO_TRACE_EVENT(source, what, value)` or
`QUIESCO_TRACE_FLOAT`; keep them out of per-sample loops.

The firmware version lives in `VERSION` (`MAJOR.MINOR.PATCH`). `build.sh`
passes it to `diagnostics/FirmwareVersion.h`, the single source for the
Device Information Service, the BLE device-info payload and the debug boot
line. Any other build reports `0.0.0`; a debug build appends `-debug`.

A debug build starts with a boot line, then prints one status line per cycle:

```text
Quiesco firmware=1.0.0-debug serial=1A2B3C4D5E6F7081 reset=power-on
```

The status line looks like this:

```text
cycle=1 T=28.36C RH=50.1% P=1011.6hPa CO2=384ppm lux=1.4 noise=51.6dB
battery=3.72V screen=bento draw=yes log=#42 bme=ok failures=0 veml=ok
failures=0 scd=ok failures=0 mic=ok ... display=ok flash=ok ble=ok
```

Each device reports `ok`, `missing`, `timeout` or `read-error` with a
consecutive-failure count, so a single line tells you which part is at fault.

### Debug console and log tools

A debug build also reads single-key commands from USB serial: `i` device and
config info, `m` measure now, `d` dump the whole on-flash log as CSV, `w` 45 s
of back-to-back flash writes (config saves and empty log records) for
power-pull testing by unplugging USB, `?` help. The dump runs as its own rail cycle, a batch of records per step, so BLE
and the watchdog keep running. A release build never opens the port.

| Command | What it does |
|---|---|
| `scripts/quiesco-console.py info` | prints the `i` output |
| `scripts/quiesco-console.py stress` | runs `w` and prints its progress; unplug USB during it |
| `scripts/quiesco-console.py dump log.csv` | saves the log as CSV (invalid readings are empty cells) |
| `scripts/quiesco-console.py monitor out.txt` | prints and saves every serial line, timestamped |
| `scripts/log-report.py log.csv [--reference ref.csv]` | lost records, resets, measurement timing, battery drain and life estimate, sensor ranges and dropouts; with hand-taken reference readings, the mean difference and a suggested offset |

Uploading firmware rewrites only the MCU's internal flash; the external
flash (config, enrolled phone keys, log) survives. So a unit can run the release build on
battery for days, then take the debug build over USB just to dump its log.

### Dependencies

Every dependency is pinned to an exact version in `dependencies.lock`; nothing
floats to `latest`.

| Area | Dependency | Licence | Status |
|---|---|---|---|
| Board core | Seeeduino mbed 2.9.3 (Mbed OS, nrfx, `Wire`, `SPI`) | mixed OSS | production |
| Temperature, humidity, pressure | Adafruit BME280 2.3.0 | BSD-3 | production |
| Ambient light | Adafruit VEML7700 2.1.6 | BSD-3 | production |
| CO2 | Sensirion I2C SCD4x 1.1.0 (+ Sensirion Core 0.7.3) | BSD-3 | production |
| Graphics | Adafruit GFX 1.12.6 (+ BusIO 1.17.4, Unified Sensor 1.1.15) | BSD-3 / MIT / Apache-2.0 | production |
| E-ink | GxEPD2 1.6.9 | GPL-3.0 | production candidate; licence settled by the GPL-3.0 release (§8) |
| BLE | ArduinoBLE 2.1.0 | LGPL-2.1 | gated — on-air testing pending |
| Storage | FlashDB 2.2.0, vendored with a local patch | Apache-2.0 | production |

Deliberately **not** used: a second RTOS or task scheduler, a filesystem, a
JSON/CBOR library, the Arduino `PDM` library, a standalone nrfx package (it
would conflict with the core's), NimBLE-Arduino (targets a different core),
and `analogRead()` for the battery.

---

## 3. Architecture

### One cooperative state machine

The firmware is a **single cooperative state machine** in `App::step()`,
called from `loop()`. There are no application threads.

- Classes own hardware and its lifecycle; plain structs carry readings and
  configuration; free functions hold stateless policy.
- Interrupts and BLE callbacks only copy data or set flags. All bus I/O,
  parsing, rendering and flash writes happen in `App::step()`.
- I2C and SPI are therefore serialised by construction and need no locks.
- Every wait is a **deadline**, never a busy loop. No sensor, flash operation
  or e-ink BUSY line can hold the device in a state forever.
- `delay()` on this core yields through Mbed's tickless kernel, so idle steps
  (50 ms, or 20 ms while BLE is active) put the CPU to sleep.

```text
IDLE ──deadline, sample request, or charger change──▶ POWERING
POWERING ──rail settled (1 s)──▶ INITIALIZING ──▶ ACQUIRING ──▶ WAITING
WAITING ──all sensors ready or timed out──▶ COLLECTING
COLLECTING ──▶ PERSISTING ──▶ RENDERING ──▶ SHUTTING_DOWN ──▶ IDLE

COLLECTING ──FRC requested──▶ FRC_SOAKING ──5 min──▶ FRC_EXECUTING ──▶ PERSISTING
PERSISTING ──log erase pending──▶ ERASING_LOG ──one sector per step──▶ RENDERING
RENDERING ──log sync requested──▶ SYNCING ──end, abort or 10 s stall──▶ SHUTTING_DOWN
IDLE ──config commit pending──▶ POWERING ──▶ COMMITTING_CONFIG ──▶ IDLE
IDLE ──screen/offset change──▶ POWERING ──▶ REDRAWING ──▶ RENDERING ──▶ SHUTTING_DOWN
```

A pending log sync or FRC request starts a cycle from IDLE at once, whenever
it arrived. A screen or calibration-offset change starts a **redraw** cycle
instead: it powers the rail, commits the config if needed, re-applies the
offsets to the last raw reading and draws it, without the ~11 s measurement.
A change that lands mid-cycle before the sensors are collected is simply used
by that cycle; one that lands later queues the redraw for idle. The first cycle starts immediately at boot. Its initialisation also loads the
configuration from flash, so reading the config costs no extra rail power-up.

### Source layout

```text
firmware.ino          App instance, setup() and loop() only
src/
  App.*               composition root and state machine
  board/              BoardPins (every pin), PowerDomain (the rail)
  platform/           MonotonicClock (64-bit ms), HardwareWatchdog,
                      DeviceId (FICR serial source), ResetReason,
                      I2cBusRecovery
  model/              Reading, Config, SleepWindow, FaultStatus
  drivers/            one adapter per chip: Bme280Sensor, Veml7700Sensor,
                      Scd41Sensor, PdmMicrophone, BatteryMonitor,
                      EpaperDisplay, W25Q64Flash, FlashDbPort, BleConfig
  dsp/                AcousticMetrics
  services/           EnvironmentalSampler, Scheduler, WallClock,
                      CalibrationPolicy, ConfigStore/ConfigRecord,
                      SampleLog/SampleRecord, UnitIdentity, BondTable
  protocol/           BleCodec (all BLE wire formats), LittleEndian,
                      PROTOCOL.md (the app team's BLE reference)
  storage/            FlashStorageLayout (partition map)
  ui/                 ComfortEvaluation, SleepSchedule, UiModel, Renderer, fonts/
  diagnostics/        BuildConfig, FirmwareVersion, DebugLog
  third_party/flashdb vendored FlashDB 2.2.0
smoke/sensors/        unit and factory test sketch (HARDWARE.md §8-9)
smoke/ble/            BLE on-air test sketch
tests/host/           host unit tests and fakes
scripts/              build, test, policy, dependency and font tooling
```

### Adapter rule

Third-party libraries are used only at hardware and protocol boundaries, and
**no library type crosses its adapter**. `Scd41Sensor.cpp` is the only file
that includes the Sensirion driver; `EpaperDisplay.cpp` the only one that sees
GxEPD2; `BleConfig.cpp` the only ArduinoBLE translation unit. Replacing a
sensor, the display backend or the BLE stack touches one file.

---

## 4. Features

### Power domain

`PowerDomain` is the only code that drives `3V3ON`. At boot it forces the rail
off and preloads the chip-select output latches HIGH, so no bus device can be
selected by a glitch when pins change direction. `enable()` records a 1 s
settle deadline, or 50 ms (`kBusSettleMs`) for redraw and config-commit
cycles, which touch only the e-ink and flash; drivers initialise only after `ready()`, and must tolerate
being initialised again on every cycle. `disable()` parks every
peripheral-facing pin before the rail drops.

### Scheduler

`Scheduler` measures the interval **start to start**. The next deadline
advances from the previous *scheduled* deadline, not from the end of a cycle,
so cycle duration never accumulates drift. Missed deadlines coalesce into one
immediate cycle rather than a burst. An interval change over BLE schedules the
next cycle at `now + interval`. Time comes from a 64-bit monotonic clock, so
there is no rollover to handle.

### Environmental sensors

`EnvironmentalSampler` starts all three I2C sensors together and polls them
until each is ready or has timed out, then collects whatever succeeded.

| Sensor | How it is driven | Timeout |
|---|---|---|
| BME280 | forced mode, 1× oversampling, filter off; one conversion per cycle | bounded poll of the status register |
| VEML7700 | gain 1/8, 100 ms integration; read once the integration time has passed | 150 ms |
| SCD41 | power-cycled single shot: one stabilising shot (discarded), then the measured shot, pressure-compensated from the BME280 (~10 s) | 7 s per shot |

The SCD41 follows Sensirion's power-cycled single-shot procedure ("SCD4x Low
Power Operation" application note): after power-up the first shot only
stabilises the sensor and its reading must be discarded, so every cycle takes
two shots and uses the second. Before that shot the BME280's pressure is sent
with `set_ambient_pressure`. The library's `measureSingleShot()` blocks for
5 s, which would stall BLE, so the adapter sends the command frame itself and
polls data-ready after 5 s. Earlier firmware kept the first periodic result
after power-up, which read low (field comparison: 412 ppm against 581 ppm on
a consumer reference).

The SCD41 is detected by reading its serial number, not by an empty I2C
transaction — see `HARDWARE.md` §6.3 for why that matters on this core. A
sample of 0 ppm is the sensor's invalid marker and is discarded. The SCD41's
own temperature and humidity are ignored in favour of the BME280.

### CO2 forced recalibration (FRC)

Sensirion does not support automatic self-calibration (ASC) in power-cycled
single-shot operation, so the firmware turns it off: the first probe of each
boot reads the ASC flag and, only if it is on, clears it and persists it (one
800 ms EEPROM write per sensor lifetime). Recalibration is manual. The phone
writes a target (400–2000 ppm) to the calibration control characteristic. The
next cycle runs the application note's FRC run-up for power-cycled use,
**single shots once a minute for 5 minutes**, then runs FRC from idle and
reports the result and the applied correction in the status characteristic. BLE stays serviced
throughout. The SCD41 stores the correction in its own EEPROM; the firmware
records the time, target and correction in the config (spare record bytes, so
no version bump) and publishes them as calibration state. Factory reset keeps
that record, as the sensor keeps the correction.

### Acoustic measurement

`PdmMicrophone` drives the nRF52840 PDM peripheral directly: custom pins, mono,
falling edge, +8 dB gain, ~16.1 kHz. Each cycle starts the clock **once** and
keeps it running with EasyDMA double-buffering: 8 buffers (~0.5 s) are
discarded while the mic wakes and the decimation filter settles, then
112 buffers (~7.1 s) are analysed, inside the ~10 s the cycle already waits
for the SCD41. There is no interrupt handler; the main loop polls
`EVENTS_END` about every millisecond against a 63.5 ms buffer period.

`AcousticMetrics` computes DC offset, RMS, peak and clipping (the raw
statistics `HARDWARE.md` checks and the validity policy uses), flags the
stopped-clock signature as invalid, and reports an **A-weighted equivalent
level, dB(A) Leq**, over the window, using the SPH0641's nominal sensitivity
and the PDM gain. The A-weighting filter is three biquads at 16 125 Hz: the
curve's high-pass pole pairs by bilinear transform, plus a high shelf (5.3 kHz,
−3 dB, Q 0.4) standing in for the 12.2 kHz poles above Nyquist, fitted to stay
within 0.15 dB of IEC 61672 from 20 Hz to 8 kHz; the host tests drive it with
tones. dB(A) is what sound meters, phone apps and the comfort bands use; the
earlier unweighted level read high in rooms with low-frequency hum. The level
is **not yet calibrated** against a reference meter (nominal sensitivity is
±1 dB; the nRF PDM filter's gain is assumed).

### Ambient light

`Veml7700Sensor` auto-ranges per Vishay's application note without blocking:
a coarse read at gain 1/8 and 100 ms, one sensitive re-read at gain 2 and
400 ms below ~54 lx (0.0084 lx/count), a 25 ms re-read above 10 000 counts
with Vishay's non-linearity correction. Counts are converted with the current
datasheet's 0.0042 lx/count (gain 2, 800 ms); the Adafruit library still uses
the older 0.0036, which read 14 % low. Each read waits 2.5× the integration
time. Absolute lux also depends on the case window and needs a calibration.

### Battery and charging

`BatteryMonitor` enables the divider, samples SAADC AIN7 directly, and disables
the divider again — never through `analogRead()`, which corrupts memory on
this variant. It also debounces the charger's `~CHG` line (1 s); a stable
change while idle starts a cycle, so the battery screen appears promptly.
The debounced state goes to the app in status flag bit 3.

Without a cell the charger's output swings between about 3.7 and 4.2 V, and
the firmware cannot tell that from a battery. Bench units without one are
built with `scripts/build.sh --no-battery` (`QUIESCO_NO_BATTERY`): no battery
sample, never charging, and device info tells the app there is no battery.

### Calibration offsets

The phone can set offsets for noise (±24 dB), temperature (±8 °C) and humidity
(±20 % RH). `CalibrationPolicy` applies them once, straight after collection,
so the displayed, logged, notified and synced values all agree. A temperature
offset also re-expresses humidity at the corrected temperature (same vapour
pressure, Magnus formula): the offset mostly corrects self-heating, and air
warmed inside the case reads drier. The humidity offset then covers only the
sensor's own error. Humidity is clamped to 0–100 %.

First field comparison (unit on USB, no battery, beside a consumer
reference): 27 °C / 59 % against 26 °C / 63 %. The temperature offset alone
(−1 °C) accounts for the humidity gap, so self-heating, not the humidity
sensor, is the error.

### Display

See [`UI.md`](UI.md). In short: `UiModel` rounds and judges the reading and
picks a screen; if the model equals the last one drawn, the panel is left
untouched. Otherwise `Renderer` draws it through `EpaperDisplay` using a
partial refresh, with a full refresh every `fullRefreshEveryCycles` draws
(default 10). A BUSY timeout is recorded as a display fault and retried on the
next cycle.

### Storage

The 8 MB flash is partitioned in `FlashStorageLayout.h`:

| Range | Contents |
|---|---|
| `0x000000–0x001FFF` | unused |
| `0x002000–0x003FFF` | configuration — FlashDB key-value store |
| `0x010000–0x7FEFFF` | sample log — FlashDB time-series ring |
| `0x7FF000–0x7FFFFF` | test sector: the factory test erases and programs it (`kTestSectorOffset`) |

- **Configuration** (`ConfigStore`) is versioned and validated; the same KV
  store holds the boot counter, the enrolled-phone key table (`BondTable`) and
  the sleep window (its own key, so adding it did not bump the config version
  and wipe every setting; a unit without one uses the default); FlashDB's
  key-value store makes writes power-loss-safe. A missing or invalid config record
  falls back to defaults and is rewritten. An I/O or database-mount failure
  is retried without saving defaults, exposing history, or losing phone keys. Writes happen only when the config
  changed, never in a callback: a change arriving while the rail is off
  triggers a short maintenance cycle that powers up, commits, and powers down.
  A failed write retries after 60 s.
- **Sample log** (`SampleLog`) stores one 64-byte CRC-protected record per
  cycle with a monotonic sequence number and, once the phone has synced the
  clock, a wall-clock timestamp. The oldest records are overwritten as the
  ring wraps; the ring holds about 101 000 records (~350 days at 300 s).
  FlashDB carries a local patch so an empty log formats one sector at a time
  instead of erasing 7.9 MB at first boot.
- **Log erase** (factory reset, or the erase command on its own) unmounts the
  log and erases the partition
  one sector per `App::step()`, about 90 s in all, with BLE and the watchdog
  serviced throughout; FlashDB's own clean would block for the whole erase.
  Before any reset settings are written, the KV store journals the erase
  intent and next sequence. Boot resumes a pending erase before exposing
  history. The marker clears only after a fresh sample is durable; sequence
  numbers continue across an interrupted erase. The erase command
  (`PROTOCOL.md` §9.1) is checked against the mounted log's newest sequence
  before anything is journalled, and refused, changing nothing, when the log
  holds a record later than the app's up-to sequence.

The flash is initialised only when needed and put into deep power-down before
the rail drops.

### Wall clock

The board has no RTC. The phone writes the current Unix time to the epoch
characteristic; `WallClock` anchors it to the monotonic clock and timestamps
later readings. The epoch is deliberately lost on reset, and records taken
before a sync carry a zero timestamp.

### BLE

The complete protocol (every characteristic, byte layout, validity rule,
error behaviour, notification timing and the log-download state machine) is
in [`src/protocol/PROTOCOL.md`](src/protocol/PROTOCOL.md), next to the codec.
Its worked examples are asserted byte for byte by the host tests, so the
document cannot drift from the firmware. The layouts were frozen at protocol
version 1; the current version is 5 (on-screen enrollment). Any change increments
`BleCodec::kProtocolVersion`.

In short, the device exposes the standard Device Information Service
(`0x180A`: manufacturer, model, serial, firmware and hardware revision) and one
Quiesco service with 128-bit base UUID `7A1Exxxx-8E6F-4A7A-AE32-515549455343`:

| `xxxx` | Characteristic | Access |
|---|---|---|
| `0001` | Measurement interval | R/W |
| `0002` | Core config | R/W |
| `0003` | Latest reading | R/Notify |
| `0004` | Status | R/Notify |
| `0005` | Epoch time | R/W |
| `0006` | Display screen | R/W |
| `0007` | Calibration offsets | R/W |
| `0008` | Device name | R/W |
| `0009` | Log sync control | W |
| `000A` | Log sync data | Notify |
| `000B` | Calibration control (FRC) | W |
| `000C` | Device info: protocol version, capability flags, firmware version, SCD41 serial | R |
| `000D` | Device control: factory reset, optionally erasing the log; or erase the log only | W |
| `000E` | Calibration state: last FRC time, target and correction; ASC off | R |
| `000F` | Diagnostics: last reset cause, uptime, I2C bus stuck | R |
| `0010` | Authentication challenge and proof | R/W |
| `0011` | Pending setup key id; the issued phone key for 30 s after setup | R |
| `0012` | Sleep window: whether the panel judges by time of day, UTC offset, bed and wake times | R/W |

Every characteristic except DIS, `000C`, `0010`, and `0011` is gated by
application authentication. Protocol v6 does not use link-layer pairing; the
unit refuses it.

**Identity.** The unit serial is the nRF52840's 64-bit FICR device ID as 16 hex
digits. A new or factory-reset unit is named `Quiesco XXXX` after its last four
digits. The SCD41 serial is read by the presence probe and published in device
info. A boot counter in the config KV store is incremented once per boot,
published in device info and stamped on every log record, so the app can date
records taken before the clock was synced from a synced record of the same
boot.

**Security (protocol v6).** A phone proves a random 128-bit per-phone key using
HMAC-SHA-256 and a fresh per-connection challenge. Until then, protected values
are blank and protected writes are ignored. Connection/disconnection callbacks
scrub values and pending writes immediately; a monotonically advancing session
id also protects against connection changes while HCI commands are running.

New enrollment needs USB power and proof of a key displayed only on the
physical panel. The panel shows a six-digit setup code; the app reads the
public key id, asks the user for the code, derives the setup key from it
(HMAC-SHA-256) and proves it. Only then is the phone added to the four-phone
table, and the unit hands it a random 128-bit phone key in `0011` for 30
seconds; the app keeps it in secure storage and uses it for every later
connection. A pending setup expires after three minutes, disconnect, or USB
removal. Repeated ENROL requests cannot evict phones or extend a pending
setup's lifetime. Keys enrolled under v5 (32 hex digits) keep working.
The v5 upgrade invalidated the old key-table encoding because protocol-v4
keys crossed the radio unencrypted. Existing phones must enroll again.

This is authentication, not encrypted transport (a product decision: no
encryption is needed): only an enrolled phone can read data or change
anything, but measurements remain visible to radio observers. Keys are
stored unencrypted in external flash. See the protocol's security limits and the companion app's setup-key prompt.

**Write handling.** Callbacks only copy the write into a per-characteristic
slot; `App::step()` validates and applies it through `BleCodec`. A rejected
write changes nothing, and the characteristic is republished with the value in
use, so the app confirms a write by reading it back. Two ArduinoBLE 2.1.0
behaviours would otherwise hide a write's real length:

- a fixed-length characteristic reports its full size after a short write and
  keeps the old tail bytes, so every writable characteristic is variable
  length;
- an over-long write is truncated silently, so each writable characteristic
  has one spare byte, and an over-long write arrives one byte too long and is
  rejected.

**Log download.** The phone keeps the cursor: it asks for the first sequence
it wants, the device streams what the ring still holds, and an END packet names
the next sequence to request. The device advances only after the BLE stack has
accepted a notification, so backpressure simply retries the same packet.
ArduinoBLE cannot report the negotiated MTU, so the phone sends its payload
size in the request (20 bytes is assumed otherwise).

**Availability.** With `bleAlwaysAvailable` (the default) the device
advertises continuously. With it off ("plugged in"), it advertises only while
on USB power, the same condition that opens enrolment; a phone already
connected stays connected when the cable is pulled.

### Watchdog and fault handling

- The nRF hardware watchdog runs with a **30 s** timeout. `App::step()` feeds
  it only while the current state has lasted less than its bound
  (`App::stateLimitMs`: 60 s for most states, the FRC soak + 60 s, 20 min for
  a log erase, 60 min for a log download). Every waiting state has a much
  shorter deadline of its own, so the bound is a backstop: a state that stops
  advancing while `step()` still runs is reset too, not only a hard hang.
- The reset cause (`RESETREAS`) is read and cleared at boot, printed on the
  debug boot line and published with the uptime on diagnostics (`000F`), so a
  watchdog reset is visible to support and to soak tests.
- Before `Wire.begin()` each cycle, `I2cBusRecovery` clocks SCL up to nine
  times and sends a STOP if a part is holding SDA low, bounded to about a
  millisecond. A bus that stays stuck is flagged on diagnostics.
- Each device keeps presence, timeout and consecutive-failure counters,
  published over BLE and in the debug line.
- The next rail power cycle is the first recovery action for any peripheral.

---

## 5. How it was built

The firmware was developed hardware-first, in small steps that each left a
testable image.

1. **Prove the hardware in isolation.** Before any production code, focused
   sketches established the rail polarity, the pin map, the flash command set,
   the direct SAADC battery read and the continuously clocked PDM capture.
   Every trap they uncovered is recorded in `HARDWARE.md`.
2. **Port only proven sequences.** Each production adapter copies the sequence
   a sketch proved, then adds timeouts, lifecycle control and a testable
   interface. No large generic library replaces a few verified register
   operations.
3. **Keep policy pure.** Scheduling, configuration validation, comfort bands,
   screen selection, the display-skip comparison, acoustic maths, record
   codecs, BLE encoding and fault counters are plain C++ with no Arduino
   dependency, so they run and are tested on the host.
4. **Enforce the pin rule mechanically.** `make check` fails the build if a raw
   pin number or a broken board macro appears outside `BoardPins.h`.
5. **Keep the bring-up sketch honest.** `smoke/sensors` compiles the
   production drivers through symlinks, so anything it shows is true of the
   firmware.

### Tests

`make test` builds four host binaries against small fakes of `Arduino.h`,
`ArduinoBLE.h` and a simulated W25Q64. They use doctest (vendored in
`tests/host/third_party/doctest`), so each binary takes doctest's options:
`-tc="sample log*"` runs matching test cases, `-ltc` lists them, `-s` shows
passing checks too. The binaries land in `$TMPDIR` as `quiesco-*-tests`.

- **core:** config validation, scheduler progression, missed deadlines and
  clock rollover, fault counters, comfort bands by day and night, judge-mode
  parity with the app (`tests/host/parity`: the app's own `sleep.ts` swept over
  nine days for six windows and six UTC offsets, regenerated by
  `gen-sleep-parity.mjs`), worst-metric priority, screen
  selection and battery override, the display-skip comparison, acoustic
  metrics, the sample record codec, and the battery curve;
- **power domain:** pin ordering and levels through the rail lifecycle;
- **storage and BLE:** wall clock, calibration offsets, the config record
  codec, config persistence and recovery on the flash simulator, sample log
  readout including a torn record, the stepwise log erase, unit identity, the
  BLE decoders, encoders and log packets, sleep window validation and
  persistence, and the frozen protocol's golden
  vectors, which must also appear verbatim in `PROTOCOL.md`;
- **runtime:** `App`, BLE and SCD41 state transitions: reconnect and
  enrolment, the FRC run-up, factory-reset erase recovery across reboots, the
  erase command accepted and refused (also before the log is mounted), and the
  sleep window's write, rejection, persistence and factory-reset default.

Hardware behaviour is verified with the smoke sketches and the debug status
line on a real board.

---

## 6. Coding rules

- `HARDWARE.md` is authoritative for pins and board behaviour.
- No raw pin integers or `D6`–`D10`, `D17`/`D19`, `LEDR`/`LEDG`/`LEDB` outside
  `BoardPins.h`.
- Never `analogRead()` the battery; never call `SPI.end()`; guard every
  `Wire.end()`; probe the SCD41 with a real command.
- Never stop and restart PDM between buffers.
- No bus I/O, flash writes, rendering or parsing in callbacks or interrupts.
- No library type outside its adapter.
- No indefinite waits; no heap allocation in normal operation.
- Units in names: `intervalSeconds`, `deadlineMs`, `pressurePa`.
- Report invalid data explicitly; never substitute zero.

---

## 7. Status

Working on hardware: the full measurement cycle with all seven readings, the
rail lifecycle, configuration persistence, the sample log, all display
screens, and the debug status line.

Implemented and host-tested, but **not yet verified on air**: the BLE service
(including device information and factory reset), log download, wall-clock sync
and FRC.

### Open work

What is left before release is in the roadmap in the [top-level README](../README.md#-roadmap).

---

## 8. Licence

The firmware is free software under **GPL-3.0-only** (`LICENSE`); every
project source file starts with its SPDX identifier, and `make check` fails on
one that does not. This also settles the display question: GxEPD2 is GPL-3.0,
which a GPL-3.0 firmware may link, and the LGPL-2.1 of ArduinoBLE and the core
is met by publishing the source. Third-party components and what a release
must ship with them are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

### Review regression coverage (2026-09-28)

`make verify` executes the real `App`, `BleConfig`, and `Scd41Sensor` state
machines against executable GATT, sensor, and flash fakes. Coverage includes
same-poll peer replacement, stale commands, saturating authentication failures,
setup expiration and proof-before-persistence, interrupted factory-reset erase,
startup read recovery, and delayed SCD41 data readiness. Storage tests inject a
failure at every read of a populated configuration load and require the flash
bytes, configuration, phone keys, and erase journal to survive unchanged.
