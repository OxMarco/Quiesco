# Quiesco BLE protocol

The contract between Quiesco firmware and the companion app: every service,
characteristic, byte layout, validity rule, error behaviour and timing the
app may rely on.

Protocol version: 6

The encoders and decoders live in [`BleCodec.cpp`](BleCodec.cpp), the GATT
table in [`../drivers/BleConfig.cpp`](../drivers/BleConfig.cpp). Every hex
example below is produced by the codec in the host tests
(`testProtocolGoldenVectors` in `tests/host/test_storage_ble.cpp`), which also
fails if this document stops containing it. The examples cannot drift from the
firmware.

---

## 1. Versioning and compatibility

- **Protocol version** (device info, §6.12) names the set of layouts and
  rules in this document. Any change to a frozen layout, or to the meaning of
  a field, increments it. An app must refuse to write to a device whose
  protocol version it does not know; it may still read the device
  information service.
- **Capability flags** (device info) name optional features. Bits are only
  added, never reused. A feature introduced without a layout change, for
  example a new characteristic, sets a new bit and leaves the protocol version
  alone. Before using a feature, the app checks its bit.
- **Reserved** bytes and bits are sent as zero and must be written as zero.
  The app must ignore them on read, because a later capability may define them.
- The **firmware version** is informational: support, update checks and
  release notes. Base compatibility decisions on the protocol version and the
  capability flags, never on the firmware version.

Frozen since protocol version 1: every layout in §6 and the log packet format
in §7. Version 2 added security (§3): the access rules, and the pairing and
bond flags in device info byte 9, which version 1 defined only as "debug
build". Version 3 changed what the noise fields mean: an A-weighted
equivalent level, dB(A) Leq over about 7 s, instead of an unweighted level
over 1 s. Layouts are unchanged. Version 4 replaced link-layer pairing with
app-layer authentication (§3, §6.15, §6.16): no characteristic needs an
encrypted link any more, and device info byte 9 bits 1 and 2 now mean
enrolment open and phone enrolled. Existing layouts are unchanged. Version 5 changes `0011` to public key-id
metadata only and requires proof of a secret displayed on the physical unit.
Version 6 shows a six-digit setup code instead of the 32-digit key, and hands
the phone its key in `0011` bytes 4–19 once the code is proved.

## 2. Discovery and connection

The device advertises:

- the Quiesco service UUID `7A1E0000-8E6F-4A7A-AE32-515549455343` in the
  advertising data; scan for this UUID, not for a name;
- its local name (1–16 bytes of UTF-8) in the scan response. A new or
  factory-reset unit is called `Quiesco XXXX`, where `XXXX` is the last four
  digits of its serial number.

Availability depends on the BLE policy in the core config (§6.2):

| Policy | Behaviour |
|---|---|
| always available (default) | advertises whenever no phone is connected |
| plugged in | advertises only while on USB power (charging or on a cable); a connection made on USB survives unplugging |

The device accepts one connection at a time.

**Recommended connect sequence**

1. Connect and negotiate the largest MTU the phone allows. Remember the ATT
   payload size (MTU − 3) for log download.
2. Read device info (`000C`). Stop if the protocol version is unknown.
3. Read the DIS serial number (`2A25`). This is the unit's permanent identity;
   key every stored record and log cursor by it.
4. Authenticate (§3): read the auth characteristic (`0010`) for its
   challenge and write the proof with the key stored for this serial. With no
   stored key, enrol first, which the unit accepts only on USB power.
5. Write the current time to epoch (`0005`) on **every** connection (§6.5),
   and with capability bit 14 the sleep window (`0012`) with the phone's
   current UTC offset (§6.17).
6. Subscribe to reading (`0003`) and status (`0004`); read both once for their
   current values.
7. Read the config characteristics you need.

## 3. Security

Only an enrolled phone can read the unit's data or change anything.

Versions 4 and 5 authenticate at the application layer. LE Secure Connections
pairing was dropped: on the firmware's BLE stack (ArduinoBLE 2.1.0 on the
Seeed mbed core) it failed on air with a MIC failure when encryption started,
or hung the unit. No characteristic requires an encrypted link, and the unit
refuses link-layer pairing.

**Access.** Until the connection has authenticated, every characteristic
except the Device Information Service, device info (`000C`), auth (`0010`)
and enrolment metadata (`0011`) reads as zeros, writes to it are ignored, and
nothing is notified. The unit cannot refuse a read per connection, so the
values are blank rather than denied. Once the connection authenticates, the
real values are in place and the latest reading and status are notified to
subscribers at once.

**Enrolment (version 6)** requires USB power and access to the physical
screen. ENROL creates a random 16-bit key id, a random six-digit setup code
and a random 128-bit phone key. The id alone is readable in `0011`; the code
appears only on the e-ink screen, as two rows of three digits. The app
derives the setup key from the code the user types:

```text
setup key = first 16 bytes of HMAC-SHA-256(code as six ASCII digits, "QSETUP1" ‖ key id as u16 LE)
```

and proves it with the normal challenge-response exchange. Once that proof
succeeds, the unit saves the phone key (evicting the oldest phone if four are
enrolled), authenticates the connection, and places the key id and phone key
in `0011` for 30 seconds or until disconnect. The app reads it there and uses
the phone key, never the setup key, for every later connection. A wrong code
counts as a failed proof (at most 5 per connection).

The pending setup belongs to the requesting connection. It expires after
180 seconds, disconnect, USB removal, or successful proof. Repeated ENROL
requests neither replace the pending code nor extend its lifetime. The display
returns to its normal screen when setup ends; a powered-off e-ink panel may
retain an expired code, which no longer authorizes anything.

Version 5 showed the 128-bit key itself as 32 hexadecimal digits. Keys
enrolled that way keep working under version 6.

Version 5 invalidates the legacy on-flash key-table encoding because earlier
keys crossed the radio in cleartext. Existing phones must enroll again;
configuration and environmental history survive this upgrade.

**Authentication** happens on every connection. The unit publishes a fresh
16-byte random challenge in `0010`. The phone writes PROVE with its key id and
the first 16 bytes of HMAC-SHA-256(key, "QAUTH1" ‖ challenge ‖ key id as u16
little-endian). The unit checks it in constant time. Every attempt, right or
wrong, consumes the challenge and publishes a new one, so a recorded proof
cannot be replayed; after 5 failed attempts the unit ignores further proofs
until the phone reconnects.

**Keys.** The unit keeps up to 4 enrolled phones in flash, across resets and
power loss. Enrolling a fifth evicts the one enrolled longest ago. Factory
reset (§9) forgets every phone. If a phone's key is rejected (the unit was
reset, or the phone was evicted), the app deletes it and enrols again on USB
power.

**Limits.** Application authentication does not encrypt sensor values or log
packets. A nearby sniffer can observe data during an authenticated connection.
The phone key crosses the radio once, in the 30 seconds after setup, so a
sniffer present during setup can capture it; the setup code has only 20 bits,
so a recorded setup proof also reveals it offline. This is a deliberate
trade of setup security for a short code. Recording any later connection
reveals nothing guessable. Someone who can see the physical setup screen can
enroll. This protocol does not provide transport encryption
or protection against an active relay; sensitive deployments need an authenticated
encrypted transport. USB power alone no longer grants ownership.

## 4. Services

### Device Information Service `0x180A` (Bluetooth SIG)

All read-only UTF-8 strings, readable without authentication.

| UUID | Characteristic | Value |
|---|---|---|
| `2A29` | Manufacturer name | `Quiesco` |
| `2A24` | Model number | `Quiesco v1` |
| `2A25` | Serial number | 16 uppercase hex digits: the nRF52840 factory device ID, stable for the life of the unit |
| `2A26` | Firmware revision | `MAJOR.MINOR.PATCH`, with `-debug` appended for development images |
| `2A27` | Hardware revision | `1.0.0` |

### Quiesco service `7A1E0000-8E6F-4A7A-AE32-515549455343`

Characteristic UUIDs are `7A1Exxxx-8E6F-4A7A-AE32-515549455343`. Every
characteristic except device info (`000C`), auth (`0010`) and enrolment metadata (`0011`)
reads as zeros and ignores writes until the connection has authenticated (§3).

| `xxxx` | Characteristic | Access | Bytes | Section |
|---|---|---|---|---|
| `0001` | Measurement interval | read, write | 4 | §6.1 |
| `0002` | Core config | read, write | 16 | §6.2 |
| `0003` | Latest reading | read, notify | 20 | §6.3 |
| `0004` | Status | read, notify | 20 | §6.4 |
| `0005` | Epoch time | read, write | 8 | §6.5 |
| `0006` | Display screen | read, write | 1 | §6.6 |
| `0007` | Calibration offsets | read, write | 12 | §6.7 |
| `0008` | Device name | read, write | 1–16 | §6.8 |
| `0009` | Log sync control | write | 10 | §6.9 |
| `000A` | Log sync data | notify | ≤ 180 | §7 |
| `000B` | Calibration control | write | 4 | §6.10 |
| `000C` | Device info | read | 20 | §6.12 |
| `000D` | Device control | write | 4 or 8, by opcode | §6.11 |
| `000E` | Calibration state | read | 12 | §6.13 |
| `000F` | Diagnostics | read | 12 | §6.14 |
| `0010` | Auth | read, write | 20; writes 2 or 20 | §6.15 |
| `0011` | Enrolment metadata | read | 20 | §6.16 |
| `0012` | Sleep window | read, write | 12 | §6.17 |

## 5. Conventions

### Encoding

- Every multi-byte field is **little-endian**.
- `f32` is IEEE 754 single precision.
- Fixed-point fields name their scale: `i16 ×100` holds 21.50 °C as 2150.
  Values are rounded to the nearest step and saturate at the type's range.
- Offsets in the layout tables are byte offsets from the start of the value.

### Writes

Use **write with response** for every characteristic.

- **Exact length.** Each write must be exactly the length in its layout
  table; the device name accepts 1–16 bytes, and device control 4 or 8 bytes
  depending on the opcode. A write of any other length is rejected. Split
  writes are not supported.
- **Rejected writes change nothing.** Validation happens on the device's main
  loop, after the GATT write has been acknowledged, so the phone always sees
  a successful write. (Before the connection authenticates, the write is
  silently ignored, §3.) The device reports a rejection by restoring the
  characteristic to the value it is actually using, so **read the
  characteristic back to confirm a config write**. Write-only commands
  (`0009`, `000B`, `000D`) show their effect through the status notification
  or the log data they trigger.
- **Last write wins.** Writes to different characteristics never displace
  each other. If a characteristic is written twice before the device takes
  the first write, only the second one is applied.
- **Latency.** A write is normally applied within 100 ms. While the e-ink
  panel refreshes, or during the FRC step, the device can take up to about
  5 s to answer any request. Do not treat a slow answer as a failure.
- Config changes are saved to flash automatically and survive resets.

### Notifications

The device notifies only subscribed characteristics. Values are also always
readable, so after a reconnect read them rather than wait for the next
notification.

| Characteristic | Notified |
|---|---|
| Latest reading | once per measurement, when the cycle finishes (about 11 s after it starts), and after a screen or offset change redraws the panel |
| Status | when each measurement finishes, when an FRC soak starts, when a log download starts and ends, on factory reset, and when a log erase starts and ends |
| Log sync data | only during a log download (§7) |

## 6. Characteristic reference

### 6.1 Measurement interval `0001`

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | interval in seconds: 60, 300, 600 or 1800 |

The next measurement is scheduled one new interval after the change. Example,
300 s:

```text
2c 01 00 00
```

### 6.2 Core config `0002`

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | config version; write back the value last read |
| 4 | u32 | measurement interval, as §6.1 |
| 8 | u32 | full e-ink refresh every N redraws, 1–1000 (default 10) |
| 12 | u8 | BLE policy: 1 = always available, 0 = plugged in (USB power only) |
| 13 | u8 | display screen, as §6.6 |
| 14 | u8 | temperature unit on the panel: 0 = °C, 1 = °F (capability bit 12; reserved, 0, before it) |
| 15 | u8 | reserved, 0 |

The config version is an internal storage version (currently 5). A write
whose version differs from the device's is rejected. Update this
characteristic only by read, modify, write. The defaults:

```text
05 00 00 00 2c 01 00 00 0a 00 00 00 01 02 00 00
```

### 6.3 Latest reading `0003`

| Offset | Type | Field | Unit |
|---|---|---|---|
| 0 | i16 ×100 | temperature | °C |
| 2 | u16 ×100 | relative humidity | % |
| 4 | u32 | pressure | Pa |
| 8 | u16 | CO2 | ppm |
| 10 | u32 ×10 | illuminance | lux |
| 14 | i16 ×10 | noise, A-weighted Leq over ~7 s | dB(A) |
| 16 | u16 ×1000 | battery | V |
| 18 | u8 | validity mask | |
| 19 | u8 | reserved, 0 | |

Validity mask bits:

| Bit | Reading |
|---|---|
| 0 | temperature |
| 1 | humidity |
| 2 | pressure |
| 3 | CO2 |
| 4 | light |
| 5 | noise |
| 6 | battery |

A clear bit means the sensor did not produce that value this cycle: it is
missing, timed out or failed. **Ignore the value of every field whose bit is
clear;** it is not a real zero. Calibration offsets (§6.7) are already
applied. Before the first measurement after boot the value is all zeros.

Example: 21.50 °C, 45.25 %, 101 325 Pa, 612 ppm, 180.4 lux, 38.6 dB,
3.912 V, everything valid:

```text
66 08 ad 11 cd 8b 01 00 64 02 0c 07 00 00 82 01 48 0f 7f 00
```

### 6.4 Status `0004`

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | validity mask of the latest reading (bits as §6.3) |
| 2 | u8 | device present mask |
| 3 | u8 | device timed-out mask |
| 4 | u8 × 8 | consecutive failures per device, saturating at 255 |
| 12 | u32 | newest log sequence; 0 = empty log |
| 16 | u8 | flags |
| 17 | u8 | FRC state |
| 18 | i16 | FRC correction, ppm (meaningful when FRC state = done) |

Device order for the masks (bit *n*) and the failure counters (offset 4 + *n*):

| *n* | Device |
|---|---|
| 0 | BME280 (temperature, humidity, pressure) |
| 1 | VEML7700 (light) |
| 2 | SCD41 (CO2) |
| 3 | battery monitor |
| 4 | microphone |
| 5 | display |
| 6 | flash |
| 7 | BLE |

Flags:

| Bit | Meaning |
|---|---|
| 0 | wall clock synced since boot |
| 1 | log download active |
| 2 | log erase pending or running (factory reset or erase command, §9) |
| 3 | charging (capability bit 11) |
| 4–7 | reserved |

Charging is the charger's debounced state as the panel last drew it; a change
starts a measurement cycle, so it follows within a few seconds. Show charge as
a level, not a percentage: while charging, the voltage reads the charger's
output and overstates the charge. A unit whose device info sets the
no-battery flag has no cell (a bench build): it never reports charging, the
battery monitor is absent and the battery reading is never valid, so the app
shows no battery at all rather than a fault.

FRC state:

| Value | State |
|---|---|
| 0 | idle |
| 1 | pending: waiting for the measurement cycle to start |
| 2 | soaking: the 5-minute FRC run-up |
| 3 | done: correction applied |
| 4 | failed: sensor absent or refused |

Example: every device present except the light sensor, which has failed
3 times; newest record 1234; clock synced; FRC done with a −23 ppm
correction:

```text
6f 00 fd 00 00 03 00 00 00 00 00 00 d2 04 00 00 01 03 e9 ff
```

### 6.5 Epoch time `0005`

| Offset | Type | Field |
|---|---|---|
| 0 | u64 | Unix time, milliseconds |

The unit has no real-time clock and loses the time on every reset. The value
must be non-zero and below 2³² seconds. The device anchors it to its internal
clock; later measurements, and log records from then on, carry wall-clock
time. Records taken before the first sync have none (§7.4). **Write the time
on every connection.**

Reading returns the last accepted value, not the current time; before any sync
it is zero. Example, 2026-01-01T00:00:00Z:

```text
00 a8 da 76 9b 01 00 00
```

### 6.6 Display screen `0006`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | 0 = face, 1 = ledger, 2 = bento (default) |

The battery screen cannot be selected: it replaces the chosen screen while
charging or when the battery is nearly empty. A change redraws the panel from the latest reading, typically 2–3 s after
the write; no new measurement is taken.

### 6.7 Calibration offsets `0007`

| Offset | Type | Field | Range |
|---|---|---|---|
| 0 | f32 | noise offset, dB | ±24 |
| 4 | f32 | temperature offset, °C | ±8 |
| 8 | f32 | humidity offset, % RH | ±20 |

The offsets are added to every later reading (humidity is clamped to
0–100 %), so the display, the log and every notification agree. A
temperature offset also re-expresses humidity at the corrected temperature
(same moisture content), because the offset mostly corrects the case's
self-heating, and warmer air reads drier. Set the temperature offset first,
then any remaining humidity offset. A change also
re-applies them to the latest reading at once: the panel redraws and the
reading characteristic notifies the corrected value; records already logged
keep the offsets they were taken with. NaN and
infinity are rejected. Reading returns the offsets in use. Example: −1.5 dB,
−2.0 °C, +3.0 % RH:

```text
00 00 c0 bf 00 00 00 c0 00 00 40 40
```

### 6.8 Device name `0008`

1–16 bytes of UTF-8, with no NUL and no control characters (0x00–0x1F, 0x7F).
The name is saved and advertised at once; a connected phone sees it in the
scan response after it disconnects. Longer names are rejected, not truncated.

### 6.9 Log sync control `0009`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 1 = start, 2 = abort |
| 1 | u8 | reserved, 0 |
| 2 | u32 | first sequence wanted (start only) |
| 6 | u16 | maximum records, 0 = no limit (start only) |
| 8 | u16 | the phone's ATT payload size, MTU − 3; 0 = assume 20 (start only) |

See §7. Example: start at sequence 1201, no limit, MTU 247:

```text
01 00 b1 04 00 00 00 00 f4 00
```

### 6.10 Calibration control `000B`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 1 = CO2 forced recalibration (FRC) |
| 1 | u8 | reserved, 0 |
| 2 | u16 | reference CO2 concentration, ppm, 400–2000 |

See §8. Example: recalibrate to 420 ppm (fresh outdoor air):

```text
01 00 a4 01
```

### 6.11 Device control `000D`

The opcode sets the length. Factory reset, 4 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 1 = factory reset |
| 1 | u8 | flags: bit 0 = also erase the sample log; bits 1–7 reserved |
| 2 | u16 | confirmation, must be `0xFAC7` |

See §9. Example, factory reset with log erase:

```text
01 01 c7 fa
```

Erase log, 8 bytes (capability bit 13):

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 2 = erase log |
| 1 | u8 | flags, reserved, 0 |
| 2 | u16 | confirmation, must be `0xFAC7` |
| 4 | u32 | up-to sequence: erase only if the log's newest sequence is ≤ this value; otherwise refuse and erase nothing |

Erases the whole sample log and nothing else (§9.1). Send the last sequence
you hold intact, so the unit never erases a record you have not downloaded:
a measurement that lands after your download makes the unit refuse. Example,
erase once everything up to record 1443 is safe on the phone:

```text
02 00 c7 fa a3 05 00 00
```

### 6.12 Device info `000C`

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | protocol version |
| 2 | u32 | capability flags |
| 6 | u8 | firmware major |
| 7 | u8 | firmware minor |
| 8 | u8 | firmware patch |
| 9 | u8 | flags: bit 0 = debug build; bit 1 = enrolment open (on USB power); bit 2 = at least one phone enrolled; bit 3 = no battery fitted (capability bit 11); bits 4–7 reserved |
| 10 | u48 | SCD41 serial number; 0 until the sensor has been read since boot |
| 16 | u32 | boot counter of the running boot; 0 until read from flash (capability bit 7) |

Capability flags:

| Bit | Feature |
|---|---|
| 0 | display screen selection (`0006`) |
| 1 | calibration offsets (`0007`) |
| 2 | CO2 forced recalibration (`000B`) |
| 3 | device rename (`0008`) |
| 4 | log download (`0009`, `000A`) |
| 5 | factory reset (`000D`) |
| 6 | log erase on factory reset |
| 7 | boot counter in device info and log records |
| 8 | calibration state (`000E`) |
| 9 | diagnostics (`000F`) |
| 10 | app-layer authentication (`0010`, `0011`) |
| 11 | charging flag in status (§6.4) and no-battery flag in device info |
| 12 | temperature unit in core config (§6.2) |
| 13 | log erase command (`000D` opcode 2, §9.1) |
| 14 | sleep window (`0012`): the panel can judge by time of day like the app |

The SCD41 serial becomes available after the first measurement cycle, about
1 s after boot, as does the boot counter; re-read if either is zero.
Device info is readable without authentication, so an app can check
compatibility and guide enrolment before it has access to anything else.
Example: protocol 6, capabilities 0x7FFF, firmware 1.2.3 release build,
enrolment open, no phone enrolled yet, SCD41 serial 0xA1B2C3D4E5F6, boot 7:

```text
06 00 ff 7f 00 00 01 02 03 02 f6 e5 d4 c3 b2 a1 07 00 00 00
```

### 6.13 Calibration state `000E`

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | time of the last successful FRC, Unix s; 0 if the clock was not synced then |
| 4 | u16 | reference used by the last FRC, ppm; 0 = no FRC recorded |
| 6 | i16 | correction the last FRC applied, ppm |
| 8 | u8 | flags: bit 0 = SCD41 automatic self-calibration confirmed off since boot |
| 9 | u8 × 3 | reserved, 0 |

This is a record for support; the correction itself lives in the sensor. The
FRC record survives resets and factory reset (the sensor keeps its correction
through both). Automatic self-calibration is switched off by the firmware:
it needs days of continuous measurement, which a sensor powered for seconds
per cycle never gets. The calibration offsets in use are in `0007`. Example:
FRC to 420 ppm at 2026-01-01T00:00:00Z that corrected by −37 ppm, ASC off:

```text
00 b9 55 69 a4 01 db ff 01 00 00 00
```

### 6.14 Diagnostics `000F`

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | cause of the last reset: nRF52840 `RESETREAS` bits; 0 = power-on or brownout |
| 4 | u32 | uptime since that reset, seconds |
| 8 | u8 | flags: bit 0 = I2C bus stuck (SDA still low after recovery) at the last measurement |
| 9 | u8 × 3 | reserved, 0 |

Reset-cause bits: 0 reset pin, 1 watchdog, 2 software reset (including a USB
firmware upload), 3 CPU lockup, 16 wake from System OFF, 20 VBUS wake. A
watchdog reset means the firmware stopped making progress; report it. Uptime
is refreshed at every measurement. Together with the boot counter, this is
what support and soak tests use to spot a unit that resets. Example: last
reset by the watchdog, up one hour:

```text
02 00 00 00 10 0e 00 00 00 00 00 00
```

### 6.15 Auth `0010`

Readable and writable without authentication (§3).

Read:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | flags: bit 0 = this connection is authenticated; bit 1 = enrolment open (on USB power); bit 2 = at least one phone enrolled; bits 3–7 reserved |
| 1 | u8 | reserved, 0 |
| 2 | u8 × 16 | challenge: random, new for every connection and after every PROVE |
| 18 | u16 | reserved, 0 |

Write ENROL (2 bytes), accepted only while enrolment is open:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 1 = ENROL |
| 1 | u8 | reserved, 0 |

Write PROVE (20 bytes):

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | opcode: 2 = PROVE |
| 1 | u8 | reserved, 0 |
| 2 | u16 | key id, from enrolment |
| 4 | u8 × 16 | proof: first 16 bytes of HMAC-SHA-256(key, "QAUTH1" ‖ challenge ‖ key id as u16 LE) |

Read it back after a PROVE: bit 0 set means the connection is authenticated;
otherwise the key was rejected and a new challenge is in place. Example read:
not authenticated, enrolment open, a phone enrolled, challenge 00 01 … 0f:

```text
06 00 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 00 00
```

ENROL:

```text
01 00
```

PROVE with key id 0x1234, key a0 a1 … af and that challenge:

```text
02 00 34 12 f0 64 2a b3 02 06 0f 2f 61 13 6b 3f 1d ab 8a 1a
```

### 6.16 Enrolment metadata `0011`

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | pending key id; 0 when no setup is pending |
| 2 | u16 | reserved, 0 |
| 4 | u8 × 16 | phone key once the setup code is proved; all zero while pending |

Always readable. While setup is pending it contains no secret. For pending
key id 0x1234:

```text
34 12 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

The phone derives the setup key from this id and the six digits entered from
the physical screen (§3), reads a fresh challenge, and sends PROVE. With code
123456 and the §6.15 example challenge:

```text
02 00 34 12 05 45 13 4e 24 62 bb 4d cb 21 37 60 44 ae ca 9e
```

Once the connection authenticates, this characteristic holds the phone key
for 30 seconds. Phone key a0 a1 … af:

```text
34 12 00 00 a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af
```

The phone stores that key and its id; it never stores the setup key. See §3
for expiration.

### 6.17 Sleep window `0012`

Capability bit 14. The user's sleep window, so the panel can judge the room
the way the app does instead of on the sleep bands at every hour.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | flags: bit 0 = the panel judges by time of day like the app; bits 1–7 reserved |
| 1 | u8 | reserved, 0 |
| 2 | i16 | the phone's local time offset from UTC, minutes, −720 … +840 |
| 4 | u16 | weekday bedtime, minutes after local midnight, 0–1439 |
| 6 | u16 | weekday wake time, 0–1439 |
| 8 | u16 | weekend bedtime (Friday and Saturday nights), or `0xFFFF` = no separate weekend times |
| 10 | u16 | weekend wake time, or `0xFFFF` |

A write with a reserved bit or byte set, a time or offset out of range, or
only one of the weekend pair `0xFFFF` is rejected and changes nothing. The
offset is a fixed number of minutes, so **rewrite it on every connection**
(after the epoch time, §6.5); a daylight-saving change then follows at the
next connection. The window is saved apart from the core config and survives
resets; factory reset restores the default. A new unit, or one reset, has the
flag clear, offset 0 and 23:30–07:00 with no weekend times:

```text
00 00 00 00 82 05 a4 01 ff ff ff ff
```

Judging by time of day, UTC+2, 23:30–07:00 every night:

```text
01 00 78 00 82 05 a4 01 ff ff ff ff
```

**What the panel does.** With bit 0 clear, or until the clock has been set
since boot (status flag bit 0), the panel judges every metric on its sleep
bands at every hour, as before this characteristic existed. With bit 0 set and
the clock synced it follows the app's rule, in local time (UTC epoch + the
offset):

- A moment belongs to the night named by its local date 12 hours earlier.
  That night's times are the weekend ones if it is a Friday or Saturday night
  and weekend times are set, otherwise the weekday ones. A bedtime before noon
  falls on the next calendar day; the window lasts from bedtime to the wrapped
  wake time. Once a night's window has ended, the next night's applies.
- **Sleep**, from 60 minutes before bedtime (the minutes to bedtime rounded
  to the nearest minute, as the app does) until wake: every metric on the
  sleep bands, with the night nudges.
- **Day**, otherwise: CO2 on the same bands; noise on hearing bands, warn
  above 70 and bad above 85 dB(A), with the nudges "loud" and "very loud";
  temperature, humidity and light shown but not judged (no frown, no nudge, no
  inverted tile, no ledger band).

The panel changes mode with the next measurement or redraw after the
boundary, so it can lag the app by up to one measurement interval. A write
that changes the mode right away redraws the panel at once.

---

## 7. Log download

The device keeps one record per measurement in a ring of about 101 000
records: roughly 350 days at 300 s, 70 days at 60 s. When the ring is full the
oldest records are overwritten. Every record has a **sequence number** that
increases by one per measurement and is never reused on that unit, even after
a log erase.

**The app keeps the cursor.** It asks for the first sequence it does not yet
have. The device streams what it still holds, then sends an END packet naming
the next sequence to ask for. There is no acknowledgement and nothing to
resume on the device.

### 7.1 State machine

```text
            write START ──▶ PENDING ──(next measurement finishes)──▶ STREAMING
                               │                                       │
            not connected when │                  DATA / FRAGMENT packets…
            the cycle finishes │                                       │
                               ▼                                       ▼
                             IDLE ◀──── END sent / ABORT / disconnect / 10 s stall
```

1. **Subscribe to `000A` first.** A notification that cannot be delivered
   stalls the session, and it ends 10 s later.
2. Write START to `0009`. The download rides a measurement: if the device is
   idle, one starts at once, and streaming begins when it finishes, about 11 s
   later (longer during an FRC soak). Status notifies with flag bit 1 set when
   streaming starts.
3. The device sends DATA packets, or FRAGMENT packets at small MTUs, in
   increasing sequence order, then one END packet. Status notifies with bit 1
   clear.
4. The session also ends **without** an END packet on ABORT, on disconnect,
   after 10 s without a delivered packet, or on a flash error. Status bit 1
   clears in every case.

A START written while a download is pending or streaming is ignored. ABORT
cancels a pending or running download. The newest record is the one taken by
the cycle that carries the download, so a full download always includes the
current measurement.

### 7.2 Packets

Every packet starts with a 12-byte header:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type: 1 = DATA, 2 = END, 3 = FRAGMENT |
| 1 | u8 | packet counter: +1 per DATA or FRAGMENT packet, wraps at 255; starts at 0 |
| 2 | u32 | DATA: sequence of the first record. FRAGMENT: the record's sequence. END: next sequence to request |
| 6 | u8 | records in this packet (DATA 1–3; FRAGMENT 1; END 0) |
| 7 | u8 | FRAGMENT: fragment index from 0; otherwise 0 |
| 8 | u16 | progress hint: records from this packet to the newest, saturating at 65535; END 0 |
| 10 | u8 | reserved, 0 |
| 11 | u8 | CRC-8 of the whole packet with this byte set to 0 |

CRC-8: polynomial 0x07, initial value 0x00, no reflection, no final XOR
(CRC-8/SMBUS). For `"123456789"` it gives `0xF4`. **Discard any packet whose
CRC fails**, then ask again from your cursor after the session ends.

**DATA** packets carry whole 52-byte records after the header; they are used
when the ATT payload is at least 64 bytes:

| ATT payload | Records per packet | Packet size |
|---|---|---|
| 20–63 | fragments (below) | ≤ 20–63 |
| 64–115 | 1 | 64 |
| 116–167 | 2 | 116 |
| 168+ | 3 | 168 |

Records within a packet, and across packets, are in increasing sequence order
but **need not be consecutive**: a record lost to power failure is skipped.
Header of a DATA packet with record 1201 as its only record:

```text
01 00 b1 04 00 00 01 00 01 00 00 06
```

**FRAGMENT** packets carry one record in consecutive chunks of up to
(payload − 12) bytes: 7 fragments of 8, 8, 8, 8, 8, 8 and 4 bytes at the
20-byte minimum. Concatenate fragments 0..n of one sequence. If any fragment
is missing or fails its CRC, drop the record. First fragment of record 1201 at
a 20-byte payload:

```text
03 00 b1 04 00 00 01 00 01 00 00 35 b1 04 00 00 7f 00 01 00
```

**END** names the next sequence to request: one past the last record sent, or
the requested sequence if nothing was sent. Example, after record 1201:

```text
02 01 b2 04 00 00 00 00 00 00 00 ab
```

**Report the payload size truthfully.** The device builds packets for the size
in the START request (clamped to 20–180). If you report more than the link
carries, packets are truncated and fail their CRC.

### 7.3 Record

| Offset | Type | Field | Unit |
|---|---|---|---|
| 0 | u32 | sequence | |
| 4 | u16 | validity mask (bits as §6.3) | |
| 6 | u16 | flags: bit 0 = wall-clock time valid | |
| 8 | u32 | wall-clock time; 0 when not synced | Unix s |
| 12 | u64 | time since the device booted | ms |
| 20 | f32 | temperature | °C |
| 24 | f32 | relative humidity | % |
| 28 | f32 | pressure | Pa |
| 32 | f32 | CO2 | ppm |
| 36 | f32 | illuminance | lux |
| 40 | f32 | noise, A-weighted Leq over ~7 s | dB(A) |
| 44 | f32 | battery | V |
| 48 | u32 | boot counter: which boot of the unit took the record; 0 = not recorded | |

As for the live reading, ignore every value whose validity bit is clear, and
calibration offsets are already applied. The boot counter is 0 on records
written by firmware without capability bit 7, or when the counter could not
be read at boot. Record 1201, taken one hour into boot 7 at
2026-01-01T01:00:00Z with the values of the §6.3 example:

```text
b1 04 00 00 7f 00 01 00 10 c7 55 69 80 ee 36 00
00 00 00 00 00 00 ac 41 00 00 35 42 80 e6 c5 47
00 00 19 44 66 66 34 43 66 66 1a 42 35 5e 7a 40
07 00 00 00
```

### 7.4 App rules

- **Cursor.** Start at 0 for a unit you have never synced. After an END, save
  its next sequence as the cursor. Without an END (abort, disconnect,
  timeout), save one past the highest sequence received intact; nothing is
  lost by asking again, and duplicates are recognised by sequence.
- **Gaps.** If the first record received is later than requested, the ring
  overwrote the records in between (or they were lost to power failure). That
  history no longer exists.
- **Restarted log.** If status reports a newest sequence below your cursor
  − 1, the log was replaced (new flash, or erased and reset before the next
  measurement). Reset the cursor to 0.
- **Time.** A record with flag bit 0 has an absolute time. A record without it
  was taken before the phone set the clock after a reset; its only time is
  milliseconds since boot. Date it from any synced record with the **same boot
  counter**: wall time = synced wall time + (its ms since boot − synced ms
  since boot). Records from a boot in which the clock was never set cannot be
  dated. A boot counter of 0 means unknown: never treat two such records as
  the same boot.
- **Batching.** Use `maximum records` to keep a session short. A later START
  with the returned cursor continues where it stopped.

### 7.5 Example session

Phone with MTU 247 (payload 244), cursor 1201, three records on the device:

```text
phone  → 000A  subscribe
phone  → 0009  01 00 b1 04 00 00 00 00 f4 00      START 1201, no limit, 244
device → 0004  status, flags bit 1 set             (about 11 s later)
device → 000A  01 00 b1 04 00 00 03 00 03 00 00 ?? + 3 × 52-byte records
device → 000A  02 01 b4 04 00 00 00 00 00 00 00 ?? END, next = 1204
device → 0004  status, flags bit 1 clear
phone          saves cursor 1204
```

## 8. CO2 forced recalibration

Automatic self-calibration cannot work on a sensor powered for a few seconds
per measurement, so the app must recalibrate CO2 now and then against a known
reference: outdoor air is about 420 ppm.

1. Ask the user to place the unit outdoors or at an open window, and wait a
   few minutes.
2. Write `000B` with the reference concentration.
3. Status goes to **pending**, then **soaking** when the measurement starts.
   The sensor then takes one measurement a minute for 5 minutes, the run-up
   Sensirion specifies for this mode. The phone may disconnect; the device
   finishes on its own.
4. Status reports **done** with the applied correction in ppm, or **failed**.

Per Sensirion's datasheet the SCD41 stores the correction in its EEPROM
itself, so it survives the power cycles between measurements (to be confirmed
on hardware, `WORKPLAN.md` M4). The last FRC is recorded in calibration state
(§6.13).

A second FRC request while one is pending or soaking is ignored. Factory reset
cancels a pending or soaking FRC.

## 9. Factory reset and log erase

Writing `000D` with a valid confirmation:

- restores every config value to its default, including the device name
  (`Quiesco XXXX`) and the calibration offsets, and saves it;
- cancels a pending or soaking FRC and sets the FRC state to idle; it does
  not undo a correction already applied to the sensor, and keeps its record in
  calibration state (§6.13);
- forgets every enrolled phone (§3), including the one that sent the reset,
  whose connection stays authenticated until it disconnects; enrolling again
  needs USB power;
- starts a measurement so the display redraws with the defaults;
- with flag bit 0, also **erases the whole sample log**. Any running download
  is aborted. The erase runs during that measurement and takes about 90 s;
  status flag bit 2 stays set until it completes. The first record afterwards
  is the one that measurement took, and sequence numbers continue from where
  they were.

The device stays connected and advertises its new name after the phone
disconnects. Erase intent and the next sequence are committed before reset
settings. After power loss, boot resumes deletion before permitting history
access. The intent clears only after a fresh record is durable; even a reset
between the final sector erase and the first append cannot reuse sequences.

Factory reset also restores the default sleep window (§6.17).

### 9.1 Erasing the log only

The erase command (`000D` opcode 2, capability bit 13) runs the same erase as
a factory reset with flag bit 0, and changes nothing else: config, device
name, enrolled phones, calibration offsets and the sleep window stay as they
are.

- It is accepted only if the log's newest sequence is at most the up-to
  sequence in the request. Otherwise it is refused and nothing changes:
  status flag bit 2 never sets and every record stays.
  The check is made against the log itself; within about 11 s of a reboot,
  before the first measurement has opened the log, it is made when that
  measurement starts writing, and bit 2 sets only if it passes.
- Once accepted: any running download is aborted, a measurement starts, the
  erase runs during it (about 90 s, status flag bit 2 set throughout, status
  notified when it starts and ends), and the record that measurement took is
  the first of the fresh log. Sequence numbers continue; they are never reused.
- Erase intent and the sequence floor are journalled first, so a power loss
  mid-erase resumes the erase on the next boot, as for a factory reset.
- A request while an erase is already pending or running is ignored.

App flow: download to the end, then write the erase with the sequence of the
last record you received intact; watch status bit 2 set, then clear; after it
clears, newest sequence is the fresh record's and your cursor stays valid.
If bit 2 never sets within one measurement (about 15 s), the request was
refused: download again and retry.

## 10. Changing this protocol

- A change to any layout in §6 or §7, or to the meaning of a field or value,
  increments `kProtocolVersion` in `BleCodec.h` and the version at the top of
  this file, and updates the affected examples. The host tests fail until all
  three agree.
- A new, independent feature is a new characteristic or a new use of reserved
  bits, announced by a new capability bit. It does not change the protocol
  version.
