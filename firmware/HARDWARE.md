# Quiesco v1 — Hardware

Quiesco is a battery-powered indoor comfort monitor: CO2, temperature,
humidity, pressure, light and noise, shown on a 1.54" e-ink panel and synced
over BLE. This document describes the v1 board as the firmware sees it — what
is fitted, how it is wired, and the traps in the Arduino core that have
already cost debugging sessions.

Sources: the Altium project and schematic export
(`hardware/sources/L-1K1W1007851A.*`), the production BOM and pick-and-place
(`hardware/assembly/`), the IPC-D-356 netlist extracted from the routed board,
the Sensirion SCD4x datasheet (v1.7), and measurements on an assembled board
with the `smoke/sensors` sketch. If anything here disagrees with the board,
trust the board and fix this file.

**Board target:** Seeeduino mbed core **2.9.3**, variant
`SEEED_XIAO_NRF52840_PLUS` (FQBN `Seeeduino:mbed:xiaonRF52840Plus`).

---

## 1. Board overview

| Ref | Part | Function | Bus |
|---|---|---|---|
| U3 | Seeed XIAO nRF52840 Plus | MCU, BLE, USB, LiPo charger | — |
| U1 | Winbond W25Q64JVSSIQ | 8 MB NOR flash (config + sample log) | SPI |
| U2 | Bosch BME280 | temperature, humidity, pressure | I2C `0x76` |
| U5 | Vishay VEML7700 | ambient light | I2C `0x10` |
| U6 | Sensirion SCD41-D-R1 | CO2 (photoacoustic NDIR) | I2C `0x62` |
| U4 | Knowles SPH0641LU4H-1 | PDM MEMS microphone | PDM |
| — | GoodDisplay GDEW0154T8D | 1.54" 152×152 1-bit e-ink | SPI |
| Q1 | AO3401 (P-channel) | switched peripheral rail | — |
| Q2, L1, D1–D3 | SI1308EDL, 10 µH, MBR0530 | e-ink gate-voltage boost | — |
| J1 | 24-pin FPC | e-ink panel | — |
| J2 | JST SH 2-pin | battery (3.7 V 280 mAh LiPo) | — |
| J3 | JST 4-pin | I2C expansion (+3.3V, SDA, SCL, GND) | I2C |
| J4 | JST 4-pin | UART expansion (+3.3V, `C_TX`, `C_RX`, GND) | UART |

The fitted module is the **plain Plus**, not the Sense Plus (the BOM names
`XIAO-nRF52840-Plus`). On Seeed's modules the LSM6DS3 IMU and the onboard mic
are Sense-only, so v1 has **no IMU**; the board's own external mic (U4) is the
only microphone. Confirm the module silkscreen before writing any IMU code —
the core compiles `6D_*` pin names for both variants and will not warn you.

---

## 2. Power

```
+3.3V_PS ──┬──────────[ Q1  AO3401  P-ch ]────── +3.3V  (switched)
(XIAO 3V3) │              S            D
           ├── R10 10K ── G                      gate pulled up to source
           │              │
           │            R13 100R
           │              │
           │           3V3ON (P0.15)             R7 bleeder S–D: not fitted
```

`+3.3V_PS` is the XIAO's always-on 3.3 V output. `+3.3V` is a **switched rail**
behind Q1 and feeds every peripheral: flash, all three I2C sensors, the mic,
the e-ink panel and the I2C pull-ups. The MCU is not on it. There are no
series diodes between the XIAO and the rail.

**Polarity: `3V3ON` LOW = rail on.** R10 holds Q1's gate at its source, so:

- drive P0.15 **LOW** → Vgs ≈ −3.3 V → rail up;
- drive it HIGH **or leave it undriven** → rail off.

A pin you forgot to drive looks exactly like a pin driven HIGH: every
peripheral goes dark at once. `PowerDomain` is the only code allowed to touch
this pin.

**Rail settle.** First ACK after rail-on, measured over 8 cycles: BME280 and
VEML7700 1 ms, SCD41 12 ms. The firmware's 1000 ms settle
(`PowerDomain::kSettleMs`) is generous for all three. Redraw and
config-commit cycles read no sensor and use `kBusSettleMs` (50 ms) instead.

**Probe point.** C20 sits directly across U6's VDD/GND and is the place to
measure the switched rail. The `3.3V` test pad is `+3.3V_PS` and reads 3.3 V
whatever the rail is doing.

**Battery.** A 3.7 V **280 mAh** LiPo (`3030-3.7V-280mAh`) on J2, charged by
the XIAO. Earlier documents quoted 500 mAh; that figure is wrong. The charger's
`~CHG` status is on P0.17.

---

## 3. Buses and pin map

### I2C — one shared bus

`SCL = P0.05`, `SDA = P0.04` — the XIAO's default `Wire`. BME280, VEML7700,
SCD41 and the J3 expansion connector share it. Pull-ups are **R14/R15, 4.7 kΩ,
on the switched rail**, so they disappear with the rail and are not a
back-power path. Each sensor also has a local pull-up pair (R2/R3, R5/R6,
R8/R9) left unfitted by design.

The schematic shows alternate per-sensor pins (P0.19, P1.01, P0.09, P1.03,
P0.31) crossed out in red; they are not used. The SCD41's SCL/SDA legs meet
the bus at a corner with no junction dot, so at page zoom U6 looks
unconnected — it is not; the netlist puts all four sensors on one net pair.

The firmware runs the bus at 100 kHz. Every part on it allows 400 kHz
(datasheet v1.6 raised the SCD41's limit from 100 kHz), so 100 kHz is margin,
not a requirement.

### SPI — flash and e-ink share the bus

| Net | nRF pin | Raw index | Device |
|---|---|---|---|
| `W_SCL` / `ED_SCLK` | P1.13 | 8 | shared clock |
| `W_DI` / `ED_DI` | P1.15 | 10 | shared MOSI |
| `W_DO` | P1.14 | 9 | flash MISO (the panel has none) |
| `W_NCS` | P0.02 | 0 | flash CS |
| `ED_CS` | P0.03 | 1 | e-ink CS |
| `ED_BUSY` | P0.28 | 2 | e-ink BUSY (active LOW) |
| `ED_RES` | P0.29 | 3 | e-ink reset |
| `ED_DC` | P0.10 | 34 (`D15`) | e-ink data/command |

**Park the other CS HIGH before talking to either device.** `ED_DC` is the one
pin whose raw index (34) is nowhere near its silkscreen number (15).

### Everything else

| Net | nRF pin | Raw index | Notes |
|---|---|---|---|
| `3V3ON` | P0.15 | 30 (`D11`) | rail gate, active LOW |
| `PDM_CLK` | P1.05 | 37 (`D18`) | mic clock (output) |
| `PDM_DATA` | P1.07 | 36 (`D17`) | mic data (input) |
| `VBAT_READ` | P0.31 | 35 | SAADC AIN7, battery divider |
| `VBAT_EN` | P0.14 | 29 | divider enable, active LOW |
| `~CHG` | P0.17 | 22 | charger status |
| `C_TX` / `C_RX` | P1.11 / P1.12 | 6 / 7 | UART on J4 |

All of these live in `src/board/BoardPins.h`, the only file allowed to hold a
raw pin number.

---

## 4. Devices

### SCD41 — U6 (CO2)

Verified against the Sensirion datasheet (Table 6, Figure 4) using the netlist
extracted from the routed board:

| Pin | Net | Datasheet |
|---|---|---|
| 6, 20, 21 (thermal pad) | GND | GND |
| 7 | +3.3V | VDD |
| 19 | +3.3V | VDDH (IR source, tied to VDD) |
| 9 | SCL | SCL |
| 10 | SDA | SDA |
| 1–5, 8, 11–18 | unconnected | DNC |

The footprint matches the recommended land pattern: 1.5 × 0.8 mm pads at
1.25 mm pitch, 4.0 mm from centre, 4.8 mm thermal pad, numbered
counter-clockwise from top view. The part is placed at 180° with pin 1 in the
correct corner.

**Presence must be checked with a real command.** On this core an empty
`Wire` transaction is sent as a read, and the SCD41 refuses reads when it has
nothing queued (§6.3). The firmware probes with `get_serial_number`.

**Operating mode.** The rail is cut after every cycle, which is Sensirion's
power-cycled single-shot operation: the first shot after power-up is
discarded and the second is used (~10 s). The first result after power-up
reads low; see `SOFTWARE.md` for the timing and forced recalibration.

**Decoupling is undersized.** U6 has only C19 (0.1 µF) and C20 (10 µF), with
no larger capacitor anywhere on the board. The datasheet asks for a supply
that handles **175–205 mA peaks** at 3.3 V with **< 30 mV** ripple, ideally a
dedicated LDO. This cannot stop the sensor answering (idle draw is sub-mA),
but it is the first suspect for noisy CO2 values or a reset mid-measurement.
The small 280 mAh cell makes sag worse. Worth fixing in a respin.

### BME280 — U2

Address `0x76` (0x77 never answers). Run in forced mode, one conversion per
cycle, 1× oversampling, filter off.

### VEML7700 — U5

Address `0x10`. Gain 1/8, 100 ms integration, enabled per cycle.

### Microphone — U4

SPH0641LU4H-1 on its own pins (**not** the Sense module's P1.00/P0.16).

- `SELECT` is strapped **LOW** (R4 = 0 Ω to GND, R1 pull-up not fitted), so
  the mic drives the **left** channel and must be sampled on the **falling**
  edge (`MODE.EDGE = LeftFalling`, mono). The wrong edge samples a
  tri-stated line and decodes noise. Fitting R1 instead would flip the edge.
- **The clock must run continuously.** Stop `PDM_CLK` and the mic sleeps; it
  needs tens of ms to wake and the decimation filter must refill. Start PDM
  once per capture window and double-buffer with EasyDMA. Starting and
  stopping per buffer returns 100% wake-up transient and 0% audio.

### Flash — U1

W25Q64JV, 8 MB, 4 KB sectors, JEDEC ID **`EF 40 17`**.
`00 00 00` means no power (the rail); `FF FF FF` means powered but not
selected or clocked (CS / SCK / MOSI).

### E-ink panel

GDEW0154T8D, 152 × 152, 1-bit, driven with GxEPD2's `GxEPD2_154_T8` class.
BUSY is active LOW. The panel keeps its image unpowered, so the firmware can
skip a refresh entirely when nothing changed. The case window hides the top
~7 px of the panel (see `UI.md`).

### Battery measurement

`VBAT_READ` (P0.31, AIN7) through a divider enabled by driving `VBAT_EN`
(P0.14) LOW. The firmware assumes the XIAO's 1 MΩ / 510 kΩ divider and a 3.6 V
SAADC full scale; **the ratio is not yet verified against a multimeter.**

---

## 5. Open hardware questions

- **Battery divider ratio** — confirm 1M/510k against a meter at several
  battery voltages.
- **`~CHG` behaviour** — confirm the level while charging, at charge-complete
  and unplugged.
- **Back-power with the rail off** — nothing parks SDA/SCL after `Wire.end()`;
  measure rail-off current to rule out leakage through the nRF's pins.
- **SCD41 supply** — measure rail ripple during a CO2 measurement before
  deciding whether the respin needs a dedicated LDO or bulk capacitance.

---

## 6. Arduino core traps

These are properties of the installed Seeeduino mbed 2.9.3 core, verified by
reading its source and on the board. Each one has broken real code.

### 6.1 `Dn` pin macros lie

`digitalWrite(p, …)` indexes `g_APinDescription[]` by the raw integer `p`. On
the Plus variant the extra pins are appended after the LED, IMU, PDM and QSPI
entries, and `pins_arduino.h` copies `D6`–`D10` from the non-Plus variant —
so several macros point at the wrong physical pin.

| Macro | Expands to | Lands on | Silkscreen says |
|---|---|---|---|
| `D0`–`D5` | 0–5 | P0.02, P0.03, P0.28, P0.29, P0.04, P0.05 | correct |
| `D6` | 7 | **P1.12** | P1.11 |
| `D7` | 8 | **P1.13** | P1.12 |
| `D8` | 9 | **P1.14** | P1.13 (SCK) |
| `D9` | 10 | **P1.15** | P1.14 (MISO) |
| `D10` | 11 | **P0.26 (red LED)** | P1.15 (MOSI) |
| `D11`–`D16` | 30–35 | P0.15, P0.19, P1.01, P0.09, P0.10, P0.31 | correct |
| `D17` / `D19` | 36 / 38 | P1.07 / P1.03 | **swapped** |
| `D18` | 37 | P1.05 | correct |
| `LEDR` | 12 | **P0.30 (green LED)** | red |
| `LEDG` | 13 | **P0.06 (blue LED)** | green |
| `LEDB` | 14 | **P1.08** (not an LED) | blue |

> **Rule:** never write a raw pin integer outside `BoardPins.h`, and never use
> `D6`–`D10`, `D17`/`D19` or the LED macros. Go net name → nRF pin → raw
> index. The LEDs are raw 11/12/13 and active LOW.

`SPI.begin()` and `Wire.begin()` use correct pins. Direct peripheral drivers
(PDM, SAADC) take **port pin numbers** in `PSEL`, which sidesteps the table —
but `PSEL` only routes the signal. You must also set `PIN_CNF` (PDM clock as
output, data as input), or the clock never toggles and the mic is silent.

Every sketch carries a board guard so a wrong selection fails loudly:

```c
#if !defined(ARDUINO_SEEED_XIAO_NRF52840_PLUS) && \
    !defined(ARDUINO_SEEED_XIAO_NRF52840_SENSE_PLUS)
#error "Select board: XIAO nRF52840 (Sense) Plus - Seeed nRF52 mbed core"
#endif
```

### 6.2 `SPI.end()` and a second `Wire.end()` free memory twice

`MbedSPI::end()` and `MbedI2C::end()` both `delete` their mbed object
**without nulling the pointer**.

- **SPI:** `begin()` only allocates when the pointer is null, so after one
  `end()` every later transfer runs on freed memory. Call `SPI.begin()` as
  often as you like and **never call `SPI.end()`**.
- **I2C:** `begin()` always reallocates, so a single `end()` is safe, but a
  **second `end()` in a row is a double free** that hangs the board. Guard
  every `Wire.end()` with a started flag (`EnvironmentalSampler::busStarted_`).

### 6.3 An empty `Wire` transaction is a read

`Wire.beginTransmission(a); Wire.endTransmission();` with no payload does not
send an address-only write: the core turns it into a **1-byte read**
(`libraries/Wire/Wire.cpp`, the `usedTxBuffer == 0` branch). Most parts ACK a
read at any time, so the usual I2C scan idiom appears to work. The **SCD41
NAKs its read header whenever it has no reply queued**, so the idiom reports
it missing while it is fine.

This hid a working CO2 sensor for two months of bring-up (July–September
2026): the board was declared faulty, and it took a bit-banged probe that ACKed
the write address and NAKed the read to prove otherwise. **Probe the SCD41
with a real command** (`get_serial_number`), never an empty transaction.

### 6.4 `analogRead()` on the battery pin corrupts memory

`analogRead(p)` resolves the pin through the 39-entry digital table but the
ADC object through a separate 7-entry analog table. For `PIN_VBAT` (35) it
picks the right pin and then writes an `AnalogIn*` 28 slots past the end of the
analog array, over unrelated globals. Index 6 is in bounds but points at the
wrong pin. There is no working index: **read the battery through the SAADC
directly** (AIN7). `analogReference()` is also a no-op on this variant.

### 6.5 The serial console after upload

The USB CDC port re-enumerates after every upload, so one-shot `setup()`
output is usually gone before a monitor attaches. Never gate all output on a
sensor being found; print one status line per cycle with `n/a` for missing
parts, so a blank console can only mean a dead board or the wrong port.

Because the battery keeps the XIAO powered, **unplugging USB does not reset
the board**. A board hung by firmware ignores the upload tool's
1200-baud reset; double-tap RESET, or unplug the battery as well.

---

## 7. Debugging playbook

| Symptom | Check first |
|---|---|
| Every peripheral dead, red LED lit | `3V3ON` written as a literal `11` (the red LED) instead of raw 30 / `D11` — the rail never turns on |
| Flash reads `00 00 00` | rail off |
| SPI device reads all `FF` | the other device's CS not parked HIGH, or a `D8`/`D9`/`D10` macro |
| E-ink powered, BUSY behaves, garbage or blank | `ED_DC` wrong (raw 34, not 15) |
| E-ink BUSY never releases | rail or the gate-voltage boost, not D/C |
| One I2C sensor "missing", others fine | for the SCD41, the probe (§6.3); otherwise the address, or a part holding SDA low |
| All I2C sensors missing | rail off, or a device holding SDA low (one wedged part takes down the bus) |
| Board hangs after a bus teardown | a second `Wire.end()` or any `SPI.end()` (§6.2) |
| Random corruption after a battery read | `analogRead(PIN_VBAT)` (§6.4) |
| Mic returns flat silence | rail, then `PIN_CNF` not set for the PDM pins |
| Mic loud, clipped, ignores sound (`peak` = 32767 + \|dc\|) | the PDM clock is being stopped between buffers |
| Mic decodes steady noise | wrong clock edge (v1 is falling) |

### Known-good values

- rail on = `3V3ON` LOW; switched-rail probe point = C20
- flash JEDEC = `EF 40 17`
- I2C: BME280 `0x76`, VEML7700 `0x10`, SCD41 `0x62`, run at 100 kHz
- first ACK after rail-on: BME280 1 ms, VEML7700 1 ms, SCD41 12 ms
- SCD41 first periodic result: 5.0 s after start (timeout 7 s). Superseded:
  the firmware now uses two single shots, the first discarded
- sample readings on the bench: 28–30 °C, 48–50 % RH, 1011 hPa,
  CO2 380–660 ppm, battery 3.7–4.2 V
- mic at +8 dB gain, mono, falling edge, free-running: quiet room RMS ≈ 20–120
  (peak < 250); speech or a snap RMS ≈ 400–700 (peak ≈ 3000); `dc` ≈ −490 to
  −640 on this board, `clip` = 0

---

## 8. Unit test sketch — `smoke/sensors`

Use this sketch to check a new unit (it is the factory test, §9) or any unit
that misbehaves. It exercises every part through the **production drivers**:
its `src/` is a tree of symlinks into the firmware's `src/`, so a result found
there transfers to the firmware unchanged. Build it with `make smoke` and upload:

```sh
scripts/arduino-cli.sh upload --fqbn Seeeduino:mbed:xiaonRF52840Plus \
  -p /dev/cu.usbmodem101 --input-dir .build/smoke-sensors smoke/sensors
```

Open the serial port at 115200 and **press `t`**. It runs one full measurement
cycle, then the flash, display and BLE checks, and prints a PASS/FAIL line per
part, with a hint for each failure (about 20 s):

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
  serial=0123456789ABCDEF display refresh=<ms>
  look at the panel: checkerboard, border, TEST, serial digits, black bar
  advertising as "Quiesco TEST CDEF" for 60 s: find it in nRF Connect
RESULT: PASS
```

A sensor fails if it does not answer, times out, or returns a value outside a
plausible indoor range (0–50 °C, 5–95 % RH, 800–1100 hPa, 300–5000 ppm CO2,
20–110 dB with no clipping, 3.0–4.35 V). The board checks:

- **flash**: JEDEC ID `EF 40 17`, then erase, blank check, program with an
  address-dependent pattern, verify, and erase again. Only the last sector
  (`0x7FF000`, `FlashStorageLayout::kTestSectorOffset`) is touched, so a
  unit's config and log survive.
- **display**: a full refresh of a test pattern (checkerboard, one-pixel
  border, `TEST`, the last four serial digits, a black bar). The automatic
  verdict is the BUSY handshake; **the operator must look at the panel** for
  missing rows, grey patches or a bad FPC seat. The refresh time is printed
  (not yet measured on this board: note the first units' times as the
  reference).
- **BLE**: the stack starts and advertises as `Quiesco TEST XXXX` for 60 s.
  The radio sits inside the certified module, so that is the automatic
  verdict; finding the name on a phone proves the air path, and a connection
  prints `radio OK both ways`.

The other commands narrow a failure down:

| Key | What it does |
|---|---|
| `t` | unit test: one cycle plus flash, display and BLE, PASS/FAIL per part — start here |
| `n` | flash check alone |
| `e` | display check alone (test pattern) |
| `a` | BLE check alone: advertise for 60 s |
| `c` | continuous measurement cycles through the real drivers: every reading, per-sensor state and start→ready latency, plus raw mic statistics |
| `s` | settle sweep: 8 rail-off/rail-on rounds, first ACK per address |
| `i` | I2C scan with the rail left on |
| `x` | exhaustive SCD41 probe: bit-banged scan with the nRF's I2C hardware bypassed, SDA/SCL bridge check, general-call reset, wake, and a serial-number read through the Sensirion driver |
| `d` | SCD41 persistent settings (serial, ASC, temperature offset, altitude) |
| `w` | SCD41 wake probe (rules out power-down state) |
| `m` | live monitor with the rail held on — press or flex a part to catch a cracked joint |
| `f` | bus speed sweep, 400/100/50/10 kHz |
| `b` | back-power probe: anything that ACKs with the rail off |
| `1`–`5` | drop BME280 / VEML7700 / SCD41 / mic / battery from the cycle |
| `+` / `-` | rail settle ±250 ms, without reflashing |

Only a **positive** `b` result means anything: with the rail down the sketch
supplies the nRF's internal pull-ups, so an ACK proves a parasitic power path,
while silence fits both a clean rail and a dead part.

`d` reports **ASC** (automatic self-calibration), which is on from the
factory. ASC needs days of continuous running to build a baseline, which a
rail cycled every few minutes never gives it; this device relies on forced
recalibration instead.

---

## 9. Factory procedure

Per unit, on USB, with the battery fitted. It takes about five minutes. Record
the results against the unit serial.

1. **Program the test sketch.** `make smoke`, then upload
   `.build/smoke-sensors` as in §8.
2. **Test.** Open the serial port at 115200 and press `t`. Require
   `RESULT: PASS`. Look at the panel: the test pattern must be complete and
   even. Find `Quiesco TEST XXXX` in nRF Connect (any phone), where `XXXX`
   matches the digits on the panel. Record the `serial=` line.
3. **On a FAIL**, use the hint and §7 to find the fault, rework, and repeat
   from step 2. A unit ships only after a passing `t` on the final hardware.
4. **Program the release firmware.** `make build`, then upload
   `.build/quiesco_firmware` the same way (sketch directory `.`). Check that
   the panel draws a reading screen within about 15 s.
5. **Optional: CO2 forced recalibration**, in known fresh air (outdoors, or
   by an open window, about 420 ppm) from the app or nRF Connect
   (`000B`; SOFTWARE.md, calibration). Record the correction from `000E`.
   Units that skip this ship with Sensirion's factory calibration.
6. **Label** the unit with its serial and the firmware version (`VERSION`).

The test sketch leaves the config and log in flash untouched, so a unit
returned for repair can be tested the same way without losing its history.
