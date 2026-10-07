# Quiesco v1 — Work plan to production

What is left before the firmware can ship, ordered so each milestone leaves a
working, testable image. It assumes a companion **phone app** that lets the
user select the screen, download historical data and calibrate the unit, all
over BLE.

Background: [`SOFTWARE.md`](SOFTWARE.md) (firmware, including the BLE service),
[`HARDWARE.md`](HARDWARE.md) (board), [`UI.md`](UI.md) (screens),
[`src/protocol/PROTOCOL.md`](src/protocol/PROTOCOL.md) (BLE contract).
**Picking this up? Read the [session log](#session-log) at the end first.**

## Where things stand

| Area | State |
|---|---|
| Measurement cycle, all seven readings, rail lifecycle | working on hardware |
| Screens (face, ledger, bento, battery, unavailable), partial refresh | working on hardware; BUSY timeout fix verified (M1, 4 reflashes); setup-code screen seen on hardware (enrolment 2026-10-07); day/sleep judging by the app's sleep window host-tested, not yet watched on the panel |
| Config persistence, sample log | working on hardware |
| BLE service: screen, config, readings, status, clock, log download, calibration, device info, factory reset, sleep window, log erase | **tested on air from a Mac** (`scripts/ble-test.py`, 42 checks pass); log stream paced 30 ms (0 lost, ~99 records/s); a pass with the app on an iPhone and an Android phone remains |
| App contract | **protocol v6** (v1 layouts + app-layer authentication + dB(A) noise + six-digit setup code); the app accepts v6 only; documented in [`src/protocol/PROTOCOL.md`](src/protocol/PROTOCOL.md) and pinned by golden vectors |
| Security | application-layer authentication: enrolment on USB power with a six-digit code on the panel, challenge-response on every connection; no transport encryption (product decision); **tested on air from a Mac**; refusal on battery and a fifth phone not yet tested |
| CO2 measurement | power-cycled single shot per Sensirion AN (first shot discarded, pressure-compensated); running on hardware; FRC and a reference comparison not yet done |
| Temperature/humidity | RH re-expressed for the temperature offset; **self-heating much larger than first thought**: the BME280 reads ~3.3 °C above the SCD41 in the case (~33 °C indoors on USB); needs a reference to set the default offset (M4) |
| Diagnostics and test tools | reset cause, uptime, boot counter; debug console (`i m d w`), `quiesco-console.py`, `log-report.py`, `ble-test.py`; all run on hardware |
| Factory test | `smoke/sensors` `t` covers sensors, flash, display, BLE; procedure in `HARDWARE.md` §9; PASS on the bench unit; the battery check passes without a cell on USB |
| Firmware version | set from `VERSION` (1.0.0, matching the app and hardware revision) by the build; exposed in DIS and device info |
| Licence | GPL-3.0-only; notices and licence texts shipped by `make licences`; **legal review open**: the core's Nordic files are 5-Clause (M11) |
| Firmware update | **missing** (M6 decision pending) |
| Power budget | **not measured**; no battery fitted yet |

## Definition of done

The firmware is production ready when, on production-representative units:

- the phone app can select the screen, download the full history and
  calibrate the unit, reliably, from both iOS and Android;
- only an enrolled phone can read data or change anything;
- the app can tell which firmware it is talking to, and the firmware can be
  updated in the field;
- the unit runs for a measured, documented battery life at 60 s and 300 s;
- a multi-day soak shows no drift, hang, data loss or memory growth;
- every unit passes a factory test before it ships;
- the build is reproducible and every licence obligation is met.

## Testing with what we have

There is no bench: no current meter, oscilloscope or lab references. The
tools are this computer (flash, USB serial), an **iPhone** (nRF Connect or
LightBlue), USB plug and unplug, a household thermometer, a phone sound-meter
app and a consumer **CO2 / humidity / temperature monitor** (Airvalent).
**No battery is fitted yet**, so the unit runs from USB and unplugging it is
a real power cut. The firmware
therefore reports on itself: the debug console dumps the log over USB and
`scripts/log-report.py` turns it into the checks below (see `SOFTWARE.md`,
debug console).

| Check | How, with these tools |
|---|---|
| BLE, enrolment, security (M2, M5) | iPhone with the companion app and nRF Connect: enrolment on USB power (code on the panel), refusal on battery, every characteristic, notifications. **Android is untested** until someone with an Android phone runs the same list. |
| Log download (M4) | USB dump for the data; iPhone for the protocol itself (start, END packet, resume) |
| Temperature, humidity, CO2 (M4) | Unit next to the reference monitor for a day; note the monitor's readings in a reference CSV, then `log-report.py --reference` gives the offsets. FRC outdoors, then compare CO2 again. |
| Noise (M4) | Phone sound-meter app as a rough reference (±3 dB at best); treat the result as provisional |
| Battery life (M7) | **Needs a cell fitted.** Then: release build, charge to full, unplug, run for days at 60 s, then at 300 s; dump and report. An estimate from the voltage curve, not a current profile. |
| Power pulls (M8) | Debug build, `quiesco-console.py stress`, unplug USB at a random moment during the 45 s; plug back, check `reset=power-on`, `info` (config intact) and `dump` + `log-report.py` (at most one torn record). Repeat 20+ times. Also unplug during a BLE rename and a log erase. |
| Soak (M11) | Several days on battery with the phone connecting now and then; the report shows lost records, resets and missed cycles; diagnostics show the reset cause |
| Display (M9) | Interval 60 s for a day or two (1000+ refreshes); look at the panel for ghosting |
| Resets and watchdog (M8) | Upload a debug build, check `reset=` on the boot line; a USB upload shows `soft` |

Out of reach without instruments or someone else's hands: per-state current
profile, rail ripple, back-powering through SDA/SCL with the rail off, and
fault injection with SDA held low. Once a battery is fitted, unplugging USB
no longer resets the unit, so do the power-pull runs before that. Those checks stay listed
below, marked **needs a bench**.

---

## M1 — Close the known defects

- [x] **Display BUSY timeout.** Verified 2026-10-07: four reflashes of the
  1.0.0 debug build, every first cycle `display=ok`. Second cause found 2026-09-27: `end()`
  checked BUSY after deep sleep (see the session log); fixed and verified on
  the bench unit.
  The first cycle after a reflash reported
  `display=timeout`. Likely cause, from reading GxEPD2: `init()` pulses RST
  and returns without waiting on BUSY, and `EpaperDisplay::begin()` sampled
  BUSY at once, while the controller still held it low coming out of reset
  (longer from hibernate). That failed `begin()` and skipped the frame.
  `begin()` now waits up to 200 ms for BUSY to release. **Verify:** reflash the
  debug build several times and check the first `cycle=` line shows no
  `display=timeout`; `e` in `smoke/sensors` prints the refresh time.
- [x] **Unit check on the bench unit.** Run `t` in `smoke/sensors` after every
  hardware-affecting change; it must PASS. PASS on all 8 parts 2026-10-07.
  Note: on USB the battery check passes with no cell fitted (it reads the
  charger), so it cannot catch a missing battery.

## M2 — BLE on air

The protocol exists; none of it has met a real phone. This milestone de-risks
everything the app depends on.

- [x] Run `smoke/ble` on hardware: advertise, connect, read/write/notify,
  throughput, rename, idle current. Covered on 2026-10-07 by the full
  firmware and `scripts/ble-test.py` from a Mac (CoreBluetooth): 37 checks
  pass; idle current still needs a meter (M7). Record results in `SOFTWARE.md` and mark
  ArduinoBLE `production` in `dependencies.lock`. If ArduinoBLE fails, move
  `BleConfig` to the Mbed/Cordio BLE stack bundled with the core.
- [ ] Test every characteristic from nRF Connect on **both iOS and Android**,
  including notifications and the MTU the phone negotiates. Done from a Mac
  (MTU 247, every characteristic, notifications); a pass with the app on an
  iPhone and an Android phone remains.
- [x] Malformed writes, disconnect mid-write, reconnect, and rapid repeated
  changes must never corrupt state or hang the cycle. On air 2026-10-07: 60
  malformed writes, 10 rapid renames, 5 reconnects (ready in ~2.3 s), state
  intact. ArduinoBLE reads back the bytes just written until the main loop
  answers; the app ignores that echo. The adapter already
  works around ArduinoBLE's padding of short writes and silent truncation of
  long ones, and gives each characteristic its own pending slot (see
  `SOFTWARE.md`, BLE); verify short, long and back-to-back writes on air.
- [ ] Choose advertising and connection intervals for low power, then measure
  them (M7).
- [x] Confirm the always-available BLE default against the 60 s
  configuration-window policy. Decided 2026-10-07: the user's choice in the
  app (Settings → Bluetooth: always / plugged in, core config byte 12); the
  default stays always available. The power measurement under M7 still
  informs the default later.

## M3 — App contract

The app team needs a stable, versioned interface before they build on it.

- [x] **Firmware version.** Standard Device Information Service (`0x180A`):
  manufacturer, model, hardware revision, firmware version, serial number.
  The version comes from `VERSION` through `diagnostics/FirmwareVersion.h`.
- [x] **Protocol version.** Device info (`000C`) carries the protocol version
  and capability flags.
- [x] **Unit identity.** Serial from the nRF52840 FICR device ID, in DIS and
  in the default name `Quiesco XXXX`; the SCD41 serial is in device info.
- [x] **Protocol reference.** [`src/protocol/PROTOCOL.md`](src/protocol/PROTOCOL.md);
  its examples are golden vectors the host tests check against the codec
  *and* the document.
- [x] **Freeze** the config, reading, status and log-record layouts as protocol
  version 1. The log wire record grew to 52 bytes before the freeze to reserve
  the boot counter (M4), so filling it in needs no protocol change.
- [x] **Factory reset** (`000D`, with a confirmation value): restores the
  default config and optionally erases the log in ~90 s of bounded steps.
- [x] DIS strings confirmed (2026-10-07): manufacturer `Quiesco`, model
  `Quiesco v1`, hardware revision `1.0.0`; firmware `VERSION` 1.0.0, matching
  the app's 1.0.0.

## M4 — App features in the firmware

### Screen selection

Characteristic `0006` already switches face / ledger / bento, persists the
choice, and starts a measurement cycle at once so the panel redraws.

- [x] Screen and offset changes redraw from the last reading in a redraw-only
  rail cycle (50 ms bus settle + refresh) instead of a full measurement.
  **Verify the latency on hardware**, and that flash writes and the panel
  reset still succeed after the short settle.
- [x] A change that arrives after the current cycle has collected queues a
  redraw for idle, so it no longer waits for the next interval. Log sync and
  FRC requests likewise start a cycle whenever they arrive.
- [x] Expose the full-refresh cadence only if the app will use it. It is:
  the app now exposes it (core config bytes 8–11, 1–1000 redraws, default
  10), 2026-10-07.

### Historical data download

Log sync (`0009` / `000A`) streams CRC-checked records from any sequence.

- [x] **Timestamps across resets.** A boot counter (config KV store, +1 per
  boot) is stamped on every record and sent at wire offset 48, and in device
  info; capability bit 7. `PROTOCOL.md` §6.4 gives the app the dating rule and
  tells it to sync the clock on every connect.
- [x] Measure download throughput for a full log at the negotiated MTU; set a
  target (for example, one week of 5-minute data in under 30 s). A full ring
  is about 101 000 records (5.3 MB on the wire). Measured 2026-10-07 from a
  Mac at MTU 247: unpaced streaming **lost ~45 % of records** (ArduinoBLE
  reports every notify as sent); packets are now paced 30 ms apart
  (`kSyncPacketGapMs`): 0 lost, 98 records/s streaming, so a week takes ~21 s
  plus the ~11 s measurement before the stream. A full ring is ~17 min.
- [x] Verify resume after a dropped connection: the app re-requests from its
  last sequence and gets no gaps or duplicates. On air 2026-10-07, 150 of 150
  matched. The app now also checks the packet counter and re-requests from
  the first missing record.
- [ ] Verify behaviour when the ring wraps during a download.
- [x] Decide whether the app can clear the log after a successful download.
  Decided 2026-10-07: user's choice in the app. Device control `000D`
  opcode 2 erases the log only (capability bit 13, `PROTOCOL.md` §9.1), with
  the factory reset's journalled ~90 s erase; it carries the last sequence
  the app holds and is refused, changing nothing, if the log has a later
  record. Config, phone keys, offsets and the sleep window are kept.
  **Verify on air**: an accepted erase end to end (status bit 2 set then
  clear, sequences continue), and a power pull mid-erase.
  `ble-test.py run` only checks the refusal, never a real erase.

### Calibration

- [ ] **CO2 forced recalibration.** The firmware side exists (target
  400–2000 ppm, 5-minute run-up at one shot a minute, result and correction in status). Define the
  app flow: place the unit outdoors or by an open window for a few minutes,
  press calibrate, keep the app open for the soak, show the result. Test it on
  hardware against a reference or fresh outdoor air. Sensirion's datasheet
  says FRC history is stored in the sensor's EEPROM automatically; **confirm
  on hardware that the correction survives the rail power cycles**.
- [x] **Turn SCD41 ASC off**: read on the first probe of every boot, cleared
  and persisted only if on (one EEPROM write per sensor). Reported in
  calibration state. **Verify on hardware** (read the flag back after a
  power cycle).
- [ ] **Offsets** (noise, temperature, humidity) are implemented; confirm the
  ranges suit the app's UI and that the app can read back what is applied.
- [x] **SCD41 power-cycled operation.** Sensirion's low-power AN requires
  discarding the first single shot after power-up; the firmware used that
  first reading, which read low (412 ppm against 581 ppm on the Airvalent).
  Now: stabilising shot discarded, measured shot pressure-compensated from
  the BME280, FRC run-up 5 min at one shot a minute. **Re-compare with the
  Airvalent before any FRC.**
- [x] Temperature offset re-expresses humidity at the corrected temperature
  (same vapour pressure). First comparison, on USB: +1 °C and −4 % RH against
  the Airvalent, fully explained by the +1 °C.
- [ ] **Temperature self-heating.** Measure the BME280 reading against a
  reference inside the closed case and ship a sensible default offset. It
  depends on dissipation (USB/charging vs battery, interval), so measure on
  battery at the shipping interval too; the SCD41's own temperature and
  humidity are now logged next to the BME280's for this (dump only), and
  `log-report.py` compares them by temperature and dew point.
- [ ] **Noise calibration.** Calibrate dB SPL against a reference sound level
  meter and version the constants.
- [x] Record the calibration state: last FRC time, target and correction are
  persisted with the config and readable on `000E`; offsets on `0007`.

### Verdicts by time of day (parity with the app)

On 2026-09-28 the app (Room tab, direction E) stopped judging the room the
same way all day. Its policy: sleep verdicts on all four metrics from 60 min
before bedtime until wake; by day only CO2 (same bands) and noise, the
latter on hearing bands (70 dB warn, 85 dB bad, from the EPA 24 h and NIOSH
8 h limits); temperature and humidity shown but not judged by day. The
panel still judges everything on the sleep bands at every hour, so it can
frown at 27 °C in the afternoon while the app shows nothing wrong.

- [x] **Decide** whether the panel follows the app. Decided 2026-10-07:
  user's choice in the app. The app pushes the sleep window (bed and wake
  minutes, weekday and weekend, its UTC offset, and a follow flag) to a new
  characteristic `0012` (capability bit 14, `PROTOCOL.md` §6.17), saved under
  its own flash key so the config version did not move. With the flag set and
  the clock synced, `SleepSchedule` (a port of the app's `judgeMode`, tested
  for parity against `sleep.ts`) picks day or sleep mode and
  `ComfortEvaluation` gates temperature and humidity on it; otherwise the
  panel judges on the sleep bands all day, as before.
- [x] **Daytime noise bands.** Done 2026-10-07 as the user's choice in the
  app: hearing bands (warn above 70, bad above 85 dB(A)) by day, nudges
  "loud" / "very loud" (the app's words, lowercased like every panel nudge);
  the night bands and words are unchanged. **Verify on hardware**: the day
  face with a loud room, and the switch at bedtime − 60 min.

## M5 — Security

- [x] ~~Pairing and bonding with LE Secure Connections~~, dropped: on
  ArduinoBLE 2.1.0 on the mbed core it failed on air with a MIC failure when
  encryption started, or hung the unit. The unit now refuses link-layer
  pairing and no characteristic needs an encrypted link.
- [x] **Application-layer authentication** (protocol v6, `PROTOCOL.md` §3).
  The board has no button, so product chose **USB power as the
  physical-presence gate**: enrolment is refused otherwise. On USB the panel
  shows a six-digit setup code; the app proves it (HMAC), and the unit hands
  over a random per-phone key in `0011` for 30 s. Every later connection
  proves that key with a challenge-response on `0010`.
- [x] Before authentication only DIS, device info, `0010` and `0011` are
  readable; everything else reads as zeros, ignores writes and notifies
  nothing (product: identity only).
- [x] Key management: 4 phones in flash, oldest evicted; factory reset forgets
  them. Recovery after losing every phone: plug into USB and enrol again.
- [x] Every write path (FRC included) needs an authenticated connection.
- [x] **No transport encryption** (product decision). Sensor data and log
  packets cross the radio in the clear; authentication decides who can read
  or change anything.
- [ ] **On air, iOS and Android:** enrolment on USB power, refusal on battery,
  the code on the panel accepted by the app and a wrong code rejected, the
  key surviving a reset, reconnect with a resolvable private address, a
  fifth phone evicting the oldest, and nothing but zeros reaching an
  unauthenticated subscriber. From a Mac 2026-10-07: enrolment with the panel
  code on USB, wrong key refused, zeros and no notifications before auth,
  unauthenticated writes ignored, key surviving reflashes and resets. Left:
  refusal on battery (no cell yet), a fifth phone, and the phones themselves.

## M6 — Firmware updates in the field

There is no update path today apart from USB and the Arduino tools.

- [ ] Decide the mechanism: **BLE DFU from the app** (best for users; needs a
  bootloader with BLE DFU and flash space for dual banks) or **USB only**
  (simple; users need a cable and a desktop tool).
- [ ] If BLE DFU: choose the bootloader, confirm it coexists with the mbed
  core and the 8 MB external flash layout, sign images, and test power loss
  mid-update.
- [ ] Config and log must survive an update; the config version handles
  migrations.

## M7 — Power and battery life

- [ ] Measure every state: idle advertising, connected idle, rail settle,
  sensor acquisition, SCD41 measurement, PDM capture, flash write, e-ink
  refresh, log download. **Needs a bench**; until then, the field
  battery test below.
- [ ] Confirm the rail is truly off between cycles and nothing back-powers it
  through SDA/SCL after `Wire.end()`. **Needs a bench** (meter on C20).
- [ ] Confirm USB serial and every peripheral release their sleep locks in
  release builds.
- [ ] Battery life for the 280 mAh cell at 60 s and 300 s: **field test**
  (release build on battery for days, `log-report.py`); decide the shipping
  default and whether 60 s is offered.
- [ ] Verify the battery divider ratio and the `~CHG` levels against a meter,
  and tune the low-battery threshold (3.5 V) from measurements.

## M8 — Robustness

- [x] Bounded I2C bus recovery for a stuck SDA line (nine clocks + STOP
  before `Wire.begin()`, flagged on diagnostics if it fails).
  **Test on hardware** with each sensor missing and SDA held low.
- [x] Feed the watchdog only while the current state is within its bound
  (`App::stateLimitMs`); the reset cause is recorded and published on
  diagnostics (`000F`), status being full. **Verify on hardware** that the
  bootloader does not clear `RESETREAS` before the firmware reads it.
- [ ] Force hangs in I2C, PDM, flash and e-ink BUSY; each must time out or
  reset and resume.
- [ ] Pull power at every stage of a config write and a log append on real
  hardware; the unit must boot with the old or new data, never neither.
  Doable now by unplugging USB (no battery fitted) during the `w` stress
  command; see "Testing with what we have". 3 pulls on 2026-10-07 (owner
  stopped there): config intact each time, log 1..8112 with nothing missing
  or torn. Each `w` run writes ~2,500 empty filler records to the real log.
- [ ] Brownout and battery removal must never leave flash or the panel
  unusable on the next boot.
- [ ] Measure SCD41 rail ripple during a measurement; if it is large, add a
  hardware fix to the next board revision (see `HARDWARE.md` §4).
  **Needs a bench** (oscilloscope).

## M9 — Display quality

- [ ] Run 1,000+ partial-refresh cycles and check ghosting; tune the
  full-refresh cadence (default 10).
- [ ] Check legibility of every screen through the case window at the
  intended viewing distance.

## M10 — Manufacturing

- [x] **Factory test**: flash `smoke/sensors`, run `t`, require PASS. It now
  also checks the flash (JEDEC, erase/program/verify of the unpartitioned last
  sector, so config and log survive), draws a display test pattern and
  advertises BLE as `Quiesco TEST XXXX`; `n`, `e`, `a` run each alone.
  **Run it on the bench unit.**
- [ ] Flash the release firmware, record the unit serial, and optionally run an
  initial FRC in known fresh air.
- [ ] A short written procedure for programming, testing and calibration.
  **Drafted** in `HARDWARE.md` §9; product to confirm (FRC at the factory or
  not, labelling) after the first units go through it.

## M11 — Release

- [x] **Licence.** The firmware is GPL-3.0-only (`LICENSE`, SPDX headers
  enforced by `make check`), which settles GxEPD2 and the LGPL relinking
  question.
- [x] Write `THIRD_PARTY_NOTICES.md`: every dependency with version, source,
  licence, local patches and whether it is linked (taken from a real build).
  `make licences` gathers the texts and checks versions against the lock.
- [x] Add the core's LGPL-2.1 and Mbed OS licence texts to the release
  package: the installed core ships none, so `licences/` holds them and
  `make licences` copies them (2026-10-07).
- [ ] **Legal review: Nordic 5-Clause vs GPL-3.0.** The nrfx and Nordic SDK
  files inside the core's precompiled Mbed OS carry Nordic's 5-Clause
  licence (all 203 of them), not BSD-3 as first noted: clause 4 (only with
  Nordic ICs) and clause 5 (no reverse engineering of binaries) are further
  restrictions GPL-3.0 does not allow on a combined work. Decide with a
  lawyer: a licence exception for the core, relicensing, or accepting it as
  the common Arduino-nRF practice it is.
- [ ] Multi-day soak at 60 s and 300 s with BLE connections and changes
  throughout: no drift, hangs, data loss or heap growth; check stack
  high-water marks.
- [ ] Clean rebuild from `dependencies.lock` on a fresh machine; compare binary
  size and versions. On the dev Mac 2026-10-07: installed versions match the
  lock (`make licences`), and two clean release builds are byte-identical
  (425,184 bytes). A second machine remains.
- [x] Resolve every actionable compiler warning in project code. As of
  2026-09-26 the release, debug and smoke builds (`--warnings all`) show none
  from project code; the remaining ones are inside ArduinoBLE (VLAs,
  deprecated Mbed calls) and are not ours to fix.
- [ ] Tag the release and archive the exact build command and output. The
  firmware is in the monorepo's git.

---

## Suggested order

M1 and M2 first: they prove the device and the radio before anyone builds on
them. M3 next, so the app team gets a frozen contract early and can work in
parallel. M4 and M5 together, since authentication changes how every app feature
connects. M6 and M7 need decisions and measurements that take time, so start
them early and in parallel. M8–M11 close out the release.

---

## Session log

Newest first. Each entry says what changed, what is verified, and where the
next person should start.

### 2026-10-07 — End of session: where to start next

State at hand-off: everything is committed and pushed to `master` (last
commit `171002d`). The bench unit runs the **1.0.0 debug** `--no-battery`
build and holds two phone keys (the owner's phone and the Mac's
`~/.quiesco-ble-test-key.json`). Its log holds ~6,700 empty filler records
from the M8 `w` stress runs; a log erase or factory reset with erase clears
them (along with the real history).

Open, in order:
1. **Repository visibility.** The repo is private, so GitHub Pages cannot
   publish the landing site (enabling Pages returned HTTP 500) and every
   public link to it 404s: the app's firmware-update button, the build guide,
   and the privacy policy's "source is public" line. The app links
   quiesco.rest/privacy, so this blocks store submission. Make the repo
   public (Pages is free) or host the site elsewhere. DNS points at GitHub
   Pages but lists only two of its four A records (add 185.199.110.153 and
   185.199.111.153); HTTPS follows once Pages serves the custom domain.
2. **Phones.** A new app build (`eas build --profile development`, new
   native modules) on an iPhone and an Android phone: enrolment, sync with
   the progress card, the three new unit settings, the diagnostic report.
3. **M6 decision** (BLE DFU or USB only), and the **Nordic 5-Clause legal
   review** (M11).
4. **References:** temperature offset against the Airvalent (self-heating is
   large), noise against a sound meter, FRC in fresh air.
5. Deferred by the owner: battery (M7, battery parts of M5/M8), display
   stress (M9), bench work.
6. Release chores: multi-day soak, rebuild on a second machine, flash the
   release build, factory procedure (M10), tag.

### 2026-10-07 — Log erase command and the sleep window (capability bits 13, 14)

Protocol stays 6; two new capability bits, device info now reports 0x7FFF.

- **Erase command** (`000D` opcode 2, 8 bytes, `PROTOCOL.md` §9.1): the
  factory reset's journalled erase without the reset. Accepted only if the
  log's newest sequence is ≤ the up-to sequence in the request; checked on
  the mounted log, or, in the ~11 s after boot before the first record, when
  that measurement first touches the log (status bit 2 sets only once it
  passes). The 4-byte factory reset is unchanged; the characteristic is now
  8 + 1 spare bytes and the codec checks each opcode's length.
- **Sleep window** (`0012`, 12 bytes, §6.17): `model/SleepWindow` (layout,
  validation, default), `ConfigStore` key `sleep`, `ui/SleepSchedule`
  (judge mode), day bands in `ComfortEvaluation`, `UiModel::mode`, day
  nudges and ledger bands in `Renderer`. Factory reset restores the default.
- **Tests:** judge-mode parity against the app's own `sleep.ts` (6 windows ×
  6 UTC offsets × 9 days in 15 s steps; regenerate with
  `tests/host/parity/gen-sleep-parity.mjs`), boundaries, offset changes,
  unsynced fallback, day bands and redraw comparison; codec golden vectors;
  erase accepted and refused at runtime (also before the log is mounted);
  window persistence and factory-reset default.
- `scripts/ble-test.py`: encodes and decodes both; `--self-test` checks the
  new vectors; `info` and `run` read `0012`; `run` adds a sleep-window
  write/read-back/restore with an invalid write, sleep-window malformed writes,
  and an `erase-refusal` check (up-to = newest − 1, must be refused). It never
  sends an erase that could pass.

**Verified:** `make verify`, release and debug `--no-battery` builds.
**Not on hardware or on air yet:** start with `ble-test.py run`, then an
accepted erase from the app on a unit whose log you can lose.

### 2026-10-07 — On-air tests from a Mac; 1.0.0

- Firmware `VERSION` and DIS hardware revision set to 1.0.0, matching the app.
- `scripts/ble-test.py` (bleak/CoreBluetooth) runs the M2/M4/M5 checks from a
  Mac: 37 pass, 1 warning (writes echo before auth; harmless, app ignores it).
- **Log download lost ~45 % of records**: ArduinoBLE's notify always reports
  success and the stack drops packets. Paced at 30 ms (0/15/20 ms all lost
  records, 15 ms once stalled); 0 lost since. The app now checks the packet
  counter and re-requests from the first missing record, and no longer reads
  its own PROVE echo as a refusal (which could delete a valid key).
- M1 BUSY fix verified (4 reflashes), factory test PASS, 3 power pulls clean.
- The BME280 reads ~3.3 °C above the SCD41 in the case (33 °C indoors):
  self-heating is far above the +1 °C assumed; needs a reference to set the
  default offset.
- Licence texts for the core and Mbed OS added; Nordic files are 5-Clause,
  not BSD-3: legal review opened under M11.

### 2026-10-07 — Docs aligned to protocol v6; app 1.0.0 store readiness

- **Decision (product): no transport encryption is needed.** Security is
  application-layer authentication: only an enrolled phone can read data or
  change anything. The READMEs, this plan (M5, "Where things stand") and
  `SOFTWARE.md` now describe protocol v6 (six-digit setup code, per-phone key,
  challenge-response) instead of LE Secure Connections, bonds and encryption.
- The companion app accepts protocol 6 only.
- App 1.0.0 store-readiness work: the demo mode ("Explore with demo data") is
  safe in release builds; log sync shows its progress; the debug log can be
  sent as a report by email to info@impossiblelabs.xyz.

Not verified on air: M2 and M5's on-air lists still stand. Start there.

### 2026-09-28 — App decision: verdicts gated by the sleep window

No firmware change. The app now judges the room differently by day and near
bedtime (see M4, "Verdicts by time of day"). Two things for the firmware to
pick up: whether the panel should follow the same gate (needs the sleep
window on the unit), and the daytime noise bands and nudge words, which are
new copy awaiting product's confirmation. Until then the panel is the
stricter of the two, and the app tells its users nothing about it.

### 2026-09-27 — Display false timeout and VEML7700 read-error

Trigger: the on-air session's debug log showed `display=timeout` and
`veml=read-error` on every cycle, `draw=skip` throughout, and the rendering
step taking ~2.2 s (a normal full refresh, not GxEPD2's 10 s bound).
**Verified:** `make verify`, `make build`, `make build-debug`, `make smoke`
pass with no project warnings. **Verified on the bench unit** (debug build,
two cycles): `display=ok draw=yes`, `veml=ok` at 74 / 68 lx.

- **Display (M1):** `EpaperDisplay::end()` sampled BUSY *after*
  `hibernate()`, and the UC8151 holds BUSY low in deep sleep, so every draw
  counted as a timeout. The failed frame left `hasRendered_` false, so every
  cycle forced another full refresh. BUSY is now checked after `powerOff()`
  and before deep sleep. The 200 ms reset wait in `begin()` stays.
- **Light:** the 2026-09-27 auto-range rewrite reads mid-cycle through the
  Adafruit library, whose register reads return 0xFFFF on a failed transfer.
  0xFFFF counts go down the bright path and come out above 200 klx, which is
  the only way the driver can report `read-error`. The old driver read once at
  collect time and worked. The driver now uses plain `Wire` with the status
  checked, writes whole registers (no read-modify-write), retries a failed read
  every 50 ms, and times out at 5 s instead of 2 s. **Why** the early reads
  fail is still open. On the bench: `veml=ok` means fixed; `veml=timeout` means
  the transfers fail for the whole window, so look at the bus in the first
  seconds of the cycle.
- Side note: Vishay's non-linearity polynomial goes past 200 klx for raw
  readings above ~25 klx, so direct sun reads as `read-error`. Unchanged.

### 2026-09-27 — Factory test (M10) and a likely M1 fix

**Verified:** `make verify` passes; `make build` (403 KB), `make build-debug`
and `make smoke` (sensors sketch 383 KB) compile with no project warnings.
**Nothing below has run on hardware.**

- **M1, display BUSY timeout:** likely cause found by reading GxEPD2 (see
  M1). `EpaperDisplay::begin()` now waits up to 200 ms for BUSY after reset.
  The only firmware change this session.
- **M10, factory test:** `smoke/sensors` `t` adds flash, display and BLE
  checks after the sensor cycle; new keys `n` (flash), `e` (display),
  `a` (BLE). The sketch symlinks in the production `W25Q64Flash`,
  `EpaperDisplay`, fonts, `DeviceId` and `UnitIdentity`. The flash check uses
  `FlashStorageLayout::kTestSectorOffset` (the last sector, which no
  partition owns).
- **Procedure:** `HARDWARE.md` §9, a draft for product.

Next, on the unit: upload `smoke/sensors`, press `t`, and check the panel
pattern and the name in nRF Connect. Record the refresh time in `HARDWARE.md`
§7 (known-good values). Then reflash the debug firmware a few times to
confirm M1. Then carry on with the list in the 2026-09-26 entry.

### 2026-09-27 — Measurement audit: A-weighted noise, light auto-range

Trigger: after the SCD41 fix the unit read CO2 505 / 27 °C / 59 % against
the Airvalent's 574 / 26 °C / 62 %. **Verified:** `make verify` and all
builds pass; **not yet run on hardware.**

Audit findings and changes:

- **CO2:** derivation correct after yesterday's fix. The 69 ppm gap is inside
  the two devices' combined tolerance (SCD41 ±(50 ppm + 2.5 %) ≈ ±64 ppm
  at 400–1000 ppm, plus the Airvalent's own, unpublished),
  so neither can be called wrong from indoor readings. Decide outdoors (see
  next steps).
- **Temperature/humidity:** BME280 set up per Bosch's weather-monitoring
  recommendation (forced, ×1, filter off) and read before the SCD41 heats
  the case. The −1 °C offset had not been applied; with it, RH comes out
  ~62.6 % against the Airvalent's 62 %. The panel rounds both to whole
  numbers, so compare with the debug console's `cycle=` line (2 decimals).
- **Noise: changed.** Was unweighted RMS over 1 s; now A-weighted Leq, dB(A),
  over ~7.1 s (inside the SCD41's ~10 s, no extra rail time). Filter within
  0.15 dB of IEC 61672 from 20 Hz to 8 kHz, host-tested with tones. Changes
  the meaning of the noise fields: **protocol version 3**.
- **Light: changed.** Non-blocking auto-range (Vishay AN 84323), 2.5× wait
  per read (was read exactly at 1× integration), and the current datasheet's
  0.0042 lx/count instead of Adafruit's 0.0036 (readings were 14 % low).

Next: fresh-air CO2 test with both devices, then FRC if our unit is the one
off; apply the −1 °C offset; compare noise with the phone meter in dB(A)
mode; lux against the phone's light meter for a case-window factor.

### 2026-09-26 — M3, M4, M5, M8, M11 firmware work; no-bench tooling

**Verified:** `make verify` (source policy, SPDX headers, all host tests
including the protocol golden vectors) passes; `make build`, `make
build-debug` and `make smoke` compile with no project warnings (release
image ≈ 403 KB, 49 %). **Nothing below has run on hardware or on air yet.**

Done, in the order it was built:

1. **App contract (M3).** `VERSION` → `diagnostics/FirmwareVersion.h`; DIS
   (`0x180A`); device info `000C` (protocol version, capabilities, firmware
   version, SCD41 serial, boot counter, pairing/bond flags); unit serial from
   FICR and default name `Quiesco XXXX`; factory reset `000D` (confirmation
   `0xFAC7`, optional stepwise log erase); `PROTOCOL.md` with golden vectors
   that the tests check against both the codec and the document.
2. **BLE write hardening.** ArduinoBLE pads short writes to fixed-length
   characteristics and truncates long ones silently; all writable
   characteristics are now variable length with one spare byte, and each has
   its own pending slot (last write wins, rejected writes revert).
3. **M4.** Boot counter (flash KV, +1 per boot) in records and device info;
   redraw-only rail cycle for screen/offset changes; log sync and FRC start a
   cycle whenever requested; SCD41 ASC disabled once and persisted;
   calibration state `000E` (last FRC time/target/correction, kept through
   factory reset).
4. **Security (M5), protocol v2.** Product decisions: pairing only while USB
   power is present; unpaired phones read only DIS and `000C`. LE Secure
   Connections numeric comparison, code on a new pairing screen; 4 bonds in
   flash; encrypted link required for everything else; notifications held
   back from unencrypted subscribers (ArduinoBLE does not gate CCCD writes).
5. **Robustness (M8).** Progress-based watchdog (`App::stateLimitMs`);
   reset cause (`RESETREAS`) and uptime on diagnostics `000F`; bounded I2C bus
   recovery before `Wire.begin()`.
6. **Release (M11).** Firmware licensed GPL-3.0-only (product decision),
   `LICENSE`, SPDX header on every project file enforced by `make check`;
   `THIRD_PARTY_NOTICES.md`; `make licences`; FlashDB modification notice;
   Fredoka `OFL.txt` vendored; unused Unity dependency dropped.
7. **No-bench tooling.** Debug console over USB serial (`i` info, `m`
   measure, `d` CSV dump, `w` 45 s flash-write stress for power pulls);
   `scripts/quiesco-console.py`; `scripts/log-report.py` (gaps, resets,
   timing, battery estimate, sensor ranges, reference comparison, BME280 vs
   SCD41 by temperature and dew point).
8. **Measurement fixes from the first field comparison** (unit on USB, no
   battery, beside an Airvalent: CO2 412 vs 581 ppm, 27 vs 26 °C, 59 vs 63 %):
   - SCD41 now follows Sensirion's power-cycled single-shot procedure: first
     shot after power-up discarded (the old firmware kept it, which read low),
     BME280 pressure sent before the measured shot, FRC run-up 5 min at one
     shot a minute. A cycle now takes ~11 s instead of ~7 s.
   - A temperature offset now also re-expresses humidity at the corrected
     temperature; the humidity gap was entirely the +1 °C self-heating.
   - SCD41's own T/RH logged in spare flash-record bytes (dump only; BLE wire
     unchanged) for the BME280 cross-check.

Decisions recorded: pairing gate = USB power; open reads = identity only;
licence = GPL-3.0-only; SPDX `GPL-3.0-only` (switch to `-or-later` is a
one-line change if wanted). Still open for product: DIS strings and starting
version (M3), whether the app may clear the log (M4), the update mechanism
(M6), where licence notices are shown (`THIRD_PARTY_NOTICES.md` item 4).

**Next, on the unit (owner has an iPhone, USB only, no battery, an
Airvalent CO2/T/RH monitor, a thermometer and a sound-meter app):**

1. Flash the debug build; `scripts/quiesco-console.py monitor` — check the
   boot line (`reset=`), `info`, and that the CO2 cycle completes (~11 s).
2. Leave it beside the Airvalent ≥ 1 h; compare CO2 again. Only then consider
   an FRC (`000B`), outdoors at ~420 ppm.
3. Pair from the iPhone (nRF Connect) on USB: code on the panel, confirm on
   the phone; then work through M2 and M5's on-air lists. Write the −1 °C
   offset: `7A1E0007` = `00 00 00 00 00 00 80 bf 00 00 00 00`.
4. Power pulls: `quiesco-console.py stress`, unplug mid-run, 20+ times; check
   `info`, `dump`, `log-report.py` after each.
5. A day beside the Airvalent with readings noted in a reference CSV, then
   `log-report.py log.csv --reference ref.csv` for offsets and the BME280 vs
   SCD41 cross-check.

**For agents working on the code:**

- Run `make verify` before and after every change; `make build`, `make
  build-debug`, `make smoke` for anything touching drivers or `App`.
- Any change to a BLE layout or its meaning: bump
  `BleCodec::kProtocolVersion`, the version line in `PROTOCOL.md` and the
  golden vectors in `tests/host/test_storage_ble.cpp`; the tests fail until
  all three agree. New independent features get a capability bit instead.
- New project source files need `SPDX-License-Identifier: GPL-3.0-only` on
  the first line (`make check` enforces it).
- The directory is **not under git yet**; there is no history to diff
  against, so read this log and `SOFTWARE.md` for what exists.


### 2026-09-28 — Review fixes and protocol v5

- BLE connection callbacks revoke authorization and scrub pending commands
  before the next ATT operation; authentication is scoped to a session id.
- FlashDB cannot turn a failed read into permission to erase settings.
  Nonblank bad headers fail closed; application startup retries storage faults.
- Factory-reset deletion journals intent and the sequence floor, resumes after
  reboot, and clears intent only after a fresh record is durable.
- FRC run-up polls transient not-ready responses until the seven-second deadline.
- Protocol v5 setup displays a random 128-bit secret locally; Bluetooth exposes
  only its id. Enrollment requires proof before saving/evicting keys. Legacy
  key-table v1 is rejected; configuration and samples are unchanged. The companion
  app accepts v5 and provides key entry, timeout, and cancellation.
- Runtime host tests now execute App, BLE, and SCD41 state transitions. On-air
  reconnect, display legibility, and physical power-pull validation remain bench
  checks; no firmware was flashed as part of this work.

Validation: `make verify`, release build, debug build, and both smoke sketches
passed. The companion app passed TypeScript, ESLint, and all 55 Jest tests.
The setup screen was rendered from the actual bitmap fonts and checked against
the 152×152 panel bounds. Release uses 424,496 bytes flash and 92,016 bytes of
globals (52% and 38%); build warnings are from the pinned core/dependencies.
