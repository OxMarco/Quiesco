#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""On-air BLE test harness for a Quiesco unit (src/protocol/PROTOCOL.md, v6).

Needs bleak (CoreBluetooth on macOS; also BlueZ and WinRT):

  python3 -m venv .venv && .venv/bin/pip install bleak

  scripts/ble-test.py --self-test          codec and crypto against PROTOCOL.md's
                                           golden vectors; no hardware
  scripts/ble-test.py scan                 Quiesco units in range, with RSSI
  scripts/ble-test.py info                 DIS, device info, auth and enrolment
                                           state; no key needed
  scripts/ble-test.py enrol                unit on USB power: type the six-digit
                                           code from the panel; saves the key
  scripts/ble-test.py run [--quick]        the M2/M4/M5 on-air checks; PASS, FAIL,
                                           WARN or SKIP per check, exit 1 on FAIL
  scripts/ble-test.py download log.csv     whole log as CSV for log-report.py

The unit is the first one advertising the Quiesco service, or pass --name or
--address (a CoreBluetooth UUID on macOS). Close the companion app first: the
unit takes one connection at a time. Keys live in --key-file (default
~/.quiesco-ble-test-key.json) as {serial: {"keyId": n, "key": "hex"}}.

`run` never writes a valid factory reset, never erases the log, never starts
an FRC, never selects another screen and never shortens the interval. It
renames the unit (and restores the name), writes a test sleep window (and
restores the original), sends one log erase that must be refused (its up-to
sequence is below the newest record) and starts a few log downloads; each
download rides one measurement, so it redraws the panel once. WARN marks a
result the protocol allows but a person should look at; it does not fail.
"""

import argparse
import asyncio
import contextlib
import csv
import datetime
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import statistics
import struct
import sys
import time

PROTOCOL_VERSION = 6


def quiesco_uuid(short):
    return f"7a1e{short.lower()}-8e6f-4a7a-ae32-515549455343"


def sig_uuid(short):
    return f"0000{short.lower()}-0000-1000-8000-00805f9b34fb"


SERVICE = quiesco_uuid("0000")
DIS_SERVICE = sig_uuid("180A")
INTERVAL, CORE_CONFIG, READING, STATUS = "0001", "0002", "0003", "0004"
EPOCH, SCREEN, OFFSETS, NAME = "0005", "0006", "0007", "0008"
SYNC_CONTROL, LOG_DATA, CAL_CONTROL, DEVICE_INFO = "0009", "000A", "000B", "000C"
DEVICE_CONTROL, CAL_STATE, DIAGNOSTICS, AUTH, ENROL_KEY = "000D", "000E", "000F", "0010", "0011"
SLEEP_WINDOW = "0012"
DIS = {"2A29": "manufacturer", "2A24": "model", "2A25": "serial",
       "2A26": "firmware", "2A27": "hardware"}

# PROTOCOL.md §4: short id -> (name, properties the table promises).
CHARACTERISTICS = {
    INTERVAL: ("measurement interval", {"read", "write"}),
    CORE_CONFIG: ("core config", {"read", "write"}),
    READING: ("latest reading", {"read", "notify"}),
    STATUS: ("status", {"read", "notify"}),
    EPOCH: ("epoch time", {"read", "write"}),
    SCREEN: ("display screen", {"read", "write"}),
    OFFSETS: ("calibration offsets", {"read", "write"}),
    NAME: ("device name", {"read", "write"}),
    SYNC_CONTROL: ("log sync control", {"write"}),
    LOG_DATA: ("log sync data", {"notify"}),
    CAL_CONTROL: ("calibration control", {"write"}),
    DEVICE_INFO: ("device info", {"read"}),
    DEVICE_CONTROL: ("device control", {"write"}),
    CAL_STATE: ("calibration state", {"read"}),
    DIAGNOSTICS: ("diagnostics", {"read"}),
    AUTH: ("auth", {"read", "write"}),
    ENROL_KEY: ("enrolment metadata", {"read"}),
    SLEEP_WINDOW: ("sleep window", {"read", "write"}),
}
# Blank (zeros) until the connection authenticates (§3).
PROTECTED_READS = [INTERVAL, CORE_CONFIG, READING, STATUS, EPOCH, SCREEN,
                   OFFSETS, NAME, CAL_STATE, DIAGNOSTICS]

# Capability bits (§6.12).
CAP_SCREEN, CAP_OFFSETS, CAP_FRC, CAP_RENAME, CAP_LOG = 0, 1, 2, 3, 4
CAP_RESET, CAP_ERASE, CAP_BOOT, CAP_CAL_STATE, CAP_DIAG = 5, 6, 7, 8, 9
CAP_AUTH, CAP_CHARGING, CAP_TEMP_UNIT, CAP_ERASE_COMMAND, CAP_SLEEP_WINDOW = 10, 11, 12, 13, 14
# Characteristics a unit has only with a capability bit.
CHARACTERISTIC_CAPS = {SLEEP_WINDOW: CAP_SLEEP_WINDOW}

INTERVALS_S = (60, 300, 600, 1800)
REQUEST_S = 8.0       # §5: up to ~5 s while the panel refreshes
CONNECT_S = 20.0
AUTH_WAIT_S = 12.0
AUTH_POLL_S = 0.25
ISSUED_KEY_WAIT_S = 25.0  # the unit shows the issued key for 30 s (§6.16)
LOG_START_S = 90.0    # a download rides the next measurement (§7.1)
LOG_STALL_S = 15.0    # the unit gives up after 10 s without delivery
SETTLE_S = 8.0        # a rejected write is reverted from the main loop
ERASE_WATCH_S = 20.0  # an accepted erase sets status bit 2 within a measurement

DEFAULT_KEY_FILE = os.path.expanduser("~/.quiesco-ble-test-key.json")


class HarnessError(Exception):
    pass


# ===================================================================== codec

def _check_length(what, data, length):
    if len(data) != length:
        raise HarnessError(f"{what}: expected {length} bytes, got {len(data)}")


def hexs(data):
    return " ".join(f"{b:02x}" for b in data)


def unhex(text):
    return bytes.fromhex(text.replace(" ", "").replace("\n", ""))


def decode_interval(data):
    _check_length("interval", data, 4)
    return struct.unpack("<I", data)[0]


def decode_core_config(data):
    _check_length("core config", data, 16)
    version, interval, full_refresh, policy, screen, unit, reserved = \
        struct.unpack("<IIIBBBB", data)
    return {"version": version, "interval_s": interval, "full_refresh_every": full_refresh,
            "ble_always_available": policy, "screen": screen, "temperature_unit": unit,
            "reserved": reserved}


def encode_core_config(c):
    return struct.pack("<IIIBBBB", c["version"], c["interval_s"], c["full_refresh_every"],
                       c["ble_always_available"], c["screen"], c["temperature_unit"], 0)


VALID_BITS = ["temperature", "humidity", "pressure", "co2", "light", "noise", "battery"]


def decode_reading(data):
    _check_length("reading", data, 20)
    t, h, p, co2, lux, noise, batt, mask, _ = struct.unpack("<hHIHIhHBB", data)
    values = [t / 100, h / 100, p, co2, lux / 10, noise / 10, batt / 1000]
    out = {"valid": mask}
    for bit, (name, value) in enumerate(zip(VALID_BITS, values)):
        out[name] = value if mask >> bit & 1 else None
    return out


STATUS_DEVICES = ["BME280", "VEML7700", "SCD41", "battery", "microphone",
                  "display", "flash", "BLE"]
FRC_STATES = ["idle", "pending", "soaking", "done", "failed"]


def decode_status(data):
    _check_length("status", data, 20)
    valid, present, timed_out = struct.unpack_from("<HBB", data, 0)
    newest, flags, frc, correction = struct.unpack_from("<IBBh", data, 12)
    return {"valid": valid, "present": present, "timed_out": timed_out,
            "failures": list(data[4:12]), "newest_sequence": newest,
            "clock_synced": bool(flags & 1), "download_active": bool(flags & 2),
            "erasing": bool(flags & 4), "charging": bool(flags & 8), "flags": flags,
            "frc_state": frc, "frc_correction_ppm": correction}


def encode_epoch(epoch_ms):
    epoch_ms = int(round(epoch_ms))
    if epoch_ms <= 0 or epoch_ms // 1000 >= 2 ** 32:
        raise HarnessError("epoch must be non-zero and below 2^32 s")
    return struct.pack("<Q", epoch_ms)


def decode_epoch(data):
    _check_length("epoch", data, 8)
    return struct.unpack("<Q", data)[0]


def decode_offsets(data):
    _check_length("calibration offsets", data, 12)
    noise, temp, hum = struct.unpack("<fff", data)
    return {"noise_db": noise, "temperature_c": temp, "humidity_pct": hum}


def encode_offsets(noise_db, temperature_c, humidity_pct):
    return struct.pack("<fff", noise_db, temperature_c, humidity_pct)


def encode_log_start(first_sequence, max_records, att_payload):
    return struct.pack("<BBIHH", 1, 0, first_sequence, max_records, att_payload)


def encode_log_abort():
    return struct.pack("<BBIHH", 2, 0, 0, 0, 0)


RESET_CONFIRM = 0xFAC7


def encode_erase_log(up_to_sequence):
    """§6.11 opcode 2: erase only if the newest record is <= up_to_sequence."""
    if not 0 <= up_to_sequence <= 0xFFFFFFFF:
        raise HarnessError(f"up-to sequence {up_to_sequence} out of range")
    return struct.pack("<BBHI", 2, 0, RESET_CONFIRM, up_to_sequence)


def decode_device_control(data):
    """The unit's view of a 000D write (§6.11): None when it would be refused."""
    if len(data) < 4 or struct.unpack_from("<H", data, 2)[0] != RESET_CONFIRM:
        return None
    if data[0] == 1 and len(data) == 4 and data[1] & ~1 == 0:
        return {"opcode": "factory-reset", "erase_log": bool(data[1] & 1)}
    if data[0] == 2 and len(data) == 8 and data[1] == 0:
        return {"opcode": "erase-log", "up_to_sequence": struct.unpack_from("<I", data, 4)[0]}
    return None


NO_WEEKEND = 0xFFFF


def clock_text(minutes):
    return "none" if minutes == NO_WEEKEND else f"{minutes // 60:02d}:{minutes % 60:02d}"


def encode_sleep_window(follow_app, utc_offset_min, weekday, weekend=None, flags=None):
    """§6.17; weekday and weekend are (bed, wake) minutes after local midnight.
    No validation: the run suite also builds invalid windows with it."""
    weekend_bed, weekend_wake = weekend if weekend else (NO_WEEKEND, NO_WEEKEND)
    flags = (1 if follow_app else 0) if flags is None else flags
    return struct.pack("<BBhHHHH", flags, 0, utc_offset_min, weekday[0], weekday[1],
                       weekend_bed, weekend_wake)


def decode_sleep_window(data):
    _check_length("sleep window", data, 12)
    flags, reserved, offset, bed, wake, wbed, wwake = struct.unpack("<BBhHHHH", data)
    return {"follow_app": bool(flags & 1), "flags": flags, "reserved": reserved,
            "utc_offset_min": offset, "weekday_bed": bed, "weekday_wake": wake,
            "weekend_bed": wbed, "weekend_wake": wwake,
            "summary": f"{'follows the app' if flags & 1 else 'sleep bands all day'}, "
                       f"UTC{offset:+d} min, weekday {clock_text(bed)}-{clock_text(wake)}, "
                       f"weekend " + (f"{clock_text(wbed)}-{clock_text(wwake)}"
                                      if wbed != NO_WEEKEND else "same")}


def sleep_window_valid(data):
    """What the unit accepts (§6.17)."""
    if len(data) != 12:
        return False
    w = decode_sleep_window(data)
    no_weekend = w["weekend_bed"] == NO_WEEKEND and w["weekend_wake"] == NO_WEEKEND
    return (w["flags"] & ~1 == 0 and w["reserved"] == 0
            and -720 <= w["utc_offset_min"] <= 840
            and w["weekday_bed"] < 1440 and w["weekday_wake"] < 1440
            and (no_weekend or (w["weekend_bed"] < 1440 and w["weekend_wake"] < 1440)))


CAPABILITY_NAMES = ["screen", "offsets", "frc", "rename", "log", "reset", "erase",
                    "boot-counter", "cal-state", "diagnostics", "app-auth",
                    "charging", "temp-unit", "erase-command", "sleep-window"]


def decode_device_info(data):
    _check_length("device info", data, 20)
    protocol, caps, major, minor, patch, flags = struct.unpack_from("<HIBBBB", data, 0)
    scd = int.from_bytes(data[10:16], "little")
    boot = struct.unpack_from("<I", data, 16)[0]
    return {"protocol": protocol, "capabilities": caps,
            "firmware": f"{major}.{minor}.{patch}", "debug_build": bool(flags & 1),
            "enrolment_open": bool(flags & 2), "enrolled": bool(flags & 4),
            "no_battery": bool(flags & 8), "scd41_serial": scd, "boot_counter": boot}


def has_cap(info, bit):
    return bool(info["capabilities"] >> bit & 1)


def decode_calibration_state(data):
    _check_length("calibration state", data, 12)
    epoch, ref, corr, flags = struct.unpack_from("<IHhB", data, 0)
    return {"last_frc_epoch_s": epoch, "last_frc_reference_ppm": ref,
            "last_frc_correction_ppm": corr, "asc_off": bool(flags & 1)}


RESET_CAUSES = {0: "reset pin", 1: "watchdog", 2: "software reset", 3: "CPU lockup",
                16: "wake from System OFF", 20: "VBUS wake"}


def decode_diagnostics(data):
    _check_length("diagnostics", data, 12)
    reason, uptime, flags = struct.unpack_from("<IIB", data, 0)
    causes = [name for bit, name in RESET_CAUSES.items() if reason >> bit & 1]
    return {"reset_reason": reason,
            "reset_cause": ", ".join(causes) or ("power-on or brownout" if reason == 0
                                                 else hex(reason)),
            "uptime_s": uptime, "i2c_bus_stuck": bool(flags & 1)}


def decode_auth_state(data):
    _check_length("auth", data, 20)
    return {"authenticated": bool(data[0] & 1), "enrolment_open": bool(data[0] & 2),
            "enrolled": bool(data[0] & 4), "challenge": bytes(data[2:18])}


def decode_enrol_key(data):
    """(key id, key bytes); key id 0 = no setup pending, key zeros = pending."""
    _check_length("enrolment metadata", data, 20)
    return struct.unpack_from("<H", data, 0)[0], bytes(data[4:20])


def encode_enrol():
    return bytes([1, 0])


def auth_proof(key, challenge, key_id):
    """First 16 bytes of HMAC-SHA-256(key, "QAUTH1" || challenge || key id LE)."""
    message = b"QAUTH1" + bytes(challenge) + struct.pack("<H", key_id)
    return hmac.new(bytes(key), message, hashlib.sha256).digest()[:16]


def setup_key(code, key_id):
    """§3: first 16 bytes of HMAC-SHA-256(code as six ASCII digits, "QSETUP1" || id LE)."""
    if not re.fullmatch(r"\d{6}", code):
        raise HarnessError("the setup code is six digits")
    return hmac.new(code.encode("ascii"), b"QSETUP1" + struct.pack("<H", key_id),
                    hashlib.sha256).digest()[:16]


def encode_prove(key_id, key, challenge):
    return struct.pack("<BBH", 2, 0, key_id) + auth_proof(key, challenge, key_id)


# ---------------------------------------------------------------- log (§7)

LOG_HEADER = 12
RECORD_BYTES = 52
PKT_DATA, PKT_END, PKT_FRAGMENT = 1, 2, 3
RECORD_STRUCT = struct.Struct("<IHHIQfffffffI")
assert RECORD_STRUCT.size == RECORD_BYTES


def crc8(data):
    """CRC-8/SMBUS: polynomial 0x07, init 0, no reflection, no final XOR."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def packet_crc_ok(packet):
    if len(packet) < LOG_HEADER:
        return False
    copy = bytearray(packet)
    copy[11] = 0
    return crc8(copy) == packet[11]


def parse_packet_header(packet):
    """Header dict, or None for a packet too short or failing its CRC."""
    if not packet_crc_ok(packet):
        return None
    ptype, counter, sequence, count, frag, remaining = struct.unpack_from("<BBIBBH", packet, 0)
    if ptype not in (PKT_DATA, PKT_END, PKT_FRAGMENT):
        return None
    return {"type": ptype, "counter": counter, "sequence": sequence, "count": count,
            "fragment": frag, "remaining": remaining}


def build_packet(ptype, counter, sequence, count, fragment, remaining, body=b""):
    """Test helper: a log packet as the firmware builds it (§7.2)."""
    packet = bytearray(struct.pack("<BBIBBHBB", ptype, counter, sequence, count,
                                   fragment, remaining, 0, 0)) + body
    packet[11] = crc8(packet)
    return bytes(packet)


def decode_record(wire):
    _check_length("record", wire, RECORD_BYTES)
    (seq, valid, flags, wall, uptime_ms, t, h, p, co2, lux, noise, batt,
     boot) = RECORD_STRUCT.unpack(wire)
    record = {"sequence": seq, "valid": valid, "time_valid": bool(flags & 1),
              "epoch_s": wall if flags & 1 else 0, "uptime_ms": uptime_ms, "boot": boot,
              "raw": bytes(wire)}
    for bit, (name, value) in enumerate(zip(VALID_BITS, (t, h, p, co2, lux, noise, batt))):
        record[name] = value if valid >> bit & 1 else None
    return record


class LogSession:
    """One download session (§7): packets in, records and the cursor out.
    Like the app's LogSession, but it counts what it throws away."""

    def __init__(self, requested_from):
        self.requested_from = requested_from
        self.records = []
        self.ended = False
        self.end_next = None
        self.highest = -1
        self.packets = 0
        self.bytes = 0
        self.crc_failures = 0
        self.discarded = 0     # every dropped packet, CRC failures included
        self.duplicates = 0    # records at or below the highest already taken
        self.counter_skips = 0
        self.remaining = None
        self._last_counter = None
        self._drop_fragments()

    def _drop_fragments(self):
        self._frag_seq = -1
        self._frag_next = 0
        self._frag_chunks = []

    def push(self, packet):
        if self.ended:
            return
        packet = bytes(packet)
        self.packets += 1
        self.bytes += len(packet)
        header = parse_packet_header(packet)
        if header is None:
            if len(packet) >= LOG_HEADER and not packet_crc_ok(packet):
                self.crc_failures += 1
            self.discarded += 1
            self._drop_fragments()
            return
        if header["type"] != PKT_END:
            if self._last_counter is not None and \
                    header["counter"] != (self._last_counter + 1) & 0xFF:
                self.counter_skips += 1
            self._last_counter = header["counter"]
        if header["type"] == PKT_DATA:
            self._on_data(header, packet)
        elif header["type"] == PKT_FRAGMENT:
            self._on_fragment(header, packet)
        else:
            self._drop_fragments()
            self.end_next = header["sequence"]
            self.ended = True
            self.remaining = 0

    def _on_data(self, header, packet):
        self._drop_fragments()
        count = header["count"]
        if not 1 <= count <= 3 or len(packet) < LOG_HEADER + count * RECORD_BYTES:
            self.discarded += 1
            return
        for i in range(count):
            start = LOG_HEADER + i * RECORD_BYTES
            self._accept(decode_record(packet[start:start + RECORD_BYTES]))
        self.remaining = header["remaining"]

    def _on_fragment(self, header, packet):
        if header["fragment"] == 0:
            self._drop_fragments()
            self._frag_seq = header["sequence"]
        elif header["sequence"] != self._frag_seq or header["fragment"] != self._frag_next:
            self._drop_fragments()  # a fragment went missing: drop the record
            self.discarded += 1
            return
        self._frag_chunks.append(packet[LOG_HEADER:])
        self._frag_next = header["fragment"] + 1
        self.remaining = header["remaining"]
        wire = b"".join(self._frag_chunks)
        if len(wire) >= RECORD_BYTES:
            self._drop_fragments()
            if len(wire) == RECORD_BYTES:
                self._accept(decode_record(wire))
            else:
                self.discarded += 1

    def _accept(self, record):
        if record["sequence"] <= self.highest:
            self.duplicates += 1
            return
        self.highest = record["sequence"]
        self.records.append(record)

    def next_cursor(self):
        """§7.4: END's next sequence, else one past the highest intact, else the start."""
        if self.end_next is not None:
            return self.end_next
        return self.highest + 1 if self.highest >= 0 else self.requested_from


def log_was_replaced(cursor, newest_sequence):
    return cursor > 0 and newest_sequence < cursor - 1


CSV_FIELDS = ["sequence", "boot", "epoch_s", "uptime_ms", "valid", "temperature_c",
              "humidity_pct", "pressure_pa", "co2_ppm", "lux", "noise_db", "battery_v",
              "scd_temperature_c", "scd_humidity_pct"]
# Same columns and decimals as the USB dump (src/diagnostics/DebugLog.cpp), so
# log-report.py reads either. The SCD41 cross-check columns are not on air.
CSV_VALUES = [("temperature", 2), ("humidity", 2), ("pressure", 0), ("co2", 0),
              ("light", 1), ("noise", 1), ("battery", 3)]


def record_csv_row(record):
    row = [str(record["sequence"]), str(record["boot"]), str(record["epoch_s"]),
           str(record["uptime_ms"]), str(record["valid"])]
    for name, decimals in CSV_VALUES:
        value = record[name]
        row.append("" if value is None else f"{value:.{decimals}f}")
    return row + ["", ""]


def sequence_problems(records):
    """(gaps as (lo, hi) ranges, duplicates, out-of-order) over a record list."""
    gaps, duplicates, disorder = [], 0, 0
    for a, b in zip(records, records[1:]):
        if b["sequence"] == a["sequence"]:
            duplicates += 1
        elif b["sequence"] < a["sequence"]:
            disorder += 1
        elif b["sequence"] != a["sequence"] + 1:
            gaps.append((a["sequence"] + 1, b["sequence"] - 1))
    return gaps, duplicates, disorder


# ================================================================ self-test

def self_test():
    """Every PROTOCOL.md golden vector through this file's codec. No hardware."""
    failures = []

    def expect(name, condition, detail=""):
        print(f"{'ok  ' if condition else 'FAIL'} {name}" + (f": {detail}" if detail and not condition else ""))
        if not condition:
            failures.append(name)

    def close(a, b, tol=1e-4):
        return a is not None and abs(a - b) <= tol

    vectors = {}

    def vec(name, text):
        vectors[name] = text
        return unhex(text)

    expect("interval 300 s", decode_interval(vec("interval", "2c 01 00 00")) == 300)

    raw = vec("core config", "05 00 00 00 2c 01 00 00 0a 00 00 00 01 02 00 00")
    config = decode_core_config(raw)
    expect("core config defaults decode",
           config == {"version": 5, "interval_s": 300, "full_refresh_every": 10,
                      "ble_always_available": 1, "screen": 2, "temperature_unit": 0,
                      "reserved": 0}, config)
    expect("core config round trip", encode_core_config(config) == raw)

    reading = decode_reading(vec("reading", "66 08 ad 11 cd 8b 01 00 64 02 0c 07 00 00 82 01 48 0f 7f 00"))
    expect("reading", reading["valid"] == 0x7F and close(reading["temperature"], 21.5)
           and close(reading["humidity"], 45.25) and reading["pressure"] == 101325
           and reading["co2"] == 612 and close(reading["light"], 180.4)
           and close(reading["noise"], 38.6) and close(reading["battery"], 3.912), reading)
    masked = decode_reading(unhex("66 08 ad 11 cd 8b 01 00 64 02 0c 07 00 00 82 01 48 0f 01 00"))
    expect("reading validity mask hides invalid fields",
           masked["temperature"] is not None and masked["co2"] is None)

    status = decode_status(vec("status", "6f 00 fd 00 00 03 00 00 00 00 00 00 d2 04 00 00 01 03 e9 ff"))
    expect("status", status["valid"] == 0x6F and status["present"] == 0xFD
           and status["failures"] == [0, 3, 0, 0, 0, 0, 0, 0]
           and status["newest_sequence"] == 1234 and status["clock_synced"]
           and not status["download_active"] and status["frc_state"] == 3
           and status["frc_correction_ppm"] == -23, status)

    epoch_ms = int(datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc).timestamp() * 1000)
    raw = vec("epoch", "00 a8 da 76 9b 01 00 00")
    expect("epoch encode", encode_epoch(epoch_ms) == raw)
    expect("epoch decode", decode_epoch(raw) == epoch_ms)
    for bad in (0, 2 ** 32 * 1000):
        try:
            encode_epoch(bad)
            expect(f"epoch {bad} refused", False)
        except HarnessError:
            expect(f"epoch {bad} refused", True)

    raw = vec("offsets", "00 00 c0 bf 00 00 00 c0 00 00 40 40")
    offsets = decode_offsets(raw)
    expect("offsets decode", offsets == {"noise_db": -1.5, "temperature_c": -2.0,
                                         "humidity_pct": 3.0}, offsets)
    expect("offsets encode", encode_offsets(-1.5, -2.0, 3.0) == raw)

    expect("log sync START", encode_log_start(1201, 0, 244) == vec("sync start", "01 00 b1 04 00 00 00 00 f4 00"))
    expect("log sync ABORT", encode_log_abort()[:2] == b"\x02\x00" and len(encode_log_abort()) == 10)

    info = decode_device_info(vec("device info", "06 00 ff 7f 00 00 01 02 03 02 f6 e5 d4 c3 b2 a1 07 00 00 00"))
    expect("device info", info == {"protocol": 6, "capabilities": 0x7FFF, "firmware": "1.2.3",
                                   "debug_build": False, "enrolment_open": True,
                                   "enrolled": False, "no_battery": False,
                                   "scd41_serial": 0xA1B2C3D4E5F6, "boot_counter": 7}, info)

    expect("capabilities 13 and 14 named",
           has_cap(info, CAP_ERASE_COMMAND) and has_cap(info, CAP_SLEEP_WINDOW)
           and CAPABILITY_NAMES[13:15] == ["erase-command", "sleep-window"])

    reset = vec("factory reset", "01 01 c7 fa")
    expect("factory reset decodes", decode_device_control(reset) ==
           {"opcode": "factory-reset", "erase_log": True})
    erase = vec("log erase", "02 00 c7 fa a3 05 00 00")
    expect("log erase encode (up to 1443)", encode_erase_log(1443) == erase)
    expect("log erase decode", decode_device_control(erase) ==
           {"opcode": "erase-log", "up_to_sequence": 1443})
    expect("log erase refused at 4 or 9 bytes, with flags or a bad confirmation",
           decode_device_control(erase[:4]) is None and decode_device_control(erase + b"\0") is None
           and decode_device_control(b"\x02\x01" + erase[2:]) is None
           and decode_device_control(erase[:2] + b"\xc6\xfa" + erase[4:]) is None)
    expect("factory reset refused at 8 bytes", decode_device_control(reset + bytes(4)) is None)

    window = vec("sleep window", "01 00 78 00 82 05 a4 01 ff ff ff ff")
    expect("sleep window encode (UTC+120, 23:30-07:00)",
           encode_sleep_window(True, 120, (1410, 420)) == window)
    decoded = decode_sleep_window(window)
    expect("sleep window decode", decoded["follow_app"] and decoded["utc_offset_min"] == 120
           and decoded["weekday_bed"] == 1410 and decoded["weekday_wake"] == 420
           and decoded["weekend_bed"] == NO_WEEKEND, decoded)
    default = vec("sleep window default", "00 00 00 00 82 05 a4 01 ff ff ff ff")
    expect("sleep window default", encode_sleep_window(False, 0, (1410, 420)) == default
           and sleep_window_valid(default))
    expect("negative offset is little-endian i16",
           encode_sleep_window(True, -300, (1410, 420))[2:4] == b"\xd4\xfe")
    invalid = [encode_sleep_window(True, 0, (1410, 420), flags=3),
               encode_sleep_window(True, -721, (1410, 420)),
               encode_sleep_window(True, 841, (1410, 420)),
               encode_sleep_window(True, 0, (1440, 420)),
               encode_sleep_window(True, 0, (1410, 420), (60, NO_WEEKEND)),
               window[:1] + b"\x01" + window[2:], window[:11], window + b"\0"]
    expect("invalid sleep windows refused", not any(sleep_window_valid(w) for w in invalid))
    expect("weekend pair accepted", sleep_window_valid(encode_sleep_window(True, 840, (0, 1439), (1439, 0))))

    cal = decode_calibration_state(vec("calibration state", "00 b9 55 69 a4 01 db ff 01 00 00 00"))
    expect("calibration state", cal == {"last_frc_epoch_s": epoch_ms // 1000,
                                        "last_frc_reference_ppm": 420,
                                        "last_frc_correction_ppm": -37, "asc_off": True}, cal)

    diag = decode_diagnostics(vec("diagnostics", "02 00 00 00 10 0e 00 00 00 00 00 00"))
    expect("diagnostics", diag["reset_cause"] == "watchdog" and diag["uptime_s"] == 3600
           and not diag["i2c_bus_stuck"], diag)

    challenge = bytes(range(16))
    key = bytes(range(0xA0, 0xB0))
    auth = decode_auth_state(vec("auth state", "06 00 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 00 00"))
    expect("auth state", auth == {"authenticated": False, "enrolment_open": True,
                                  "enrolled": True, "challenge": challenge}, auth)
    expect("ENROL", encode_enrol() == vec("enrol", "01 00"))
    expect("PROVE (HMAC-SHA-256 proof)", encode_prove(0x1234, key, challenge) ==
           vec("prove", "02 00 34 12 f0 64 2a b3 02 06 0f 2f 61 13 6b 3f 1d ab 8a 1a"))
    expect("enrolment metadata, pending",
           decode_enrol_key(vec("enrol pending", "34 12 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"))
           == (0x1234, bytes(16)))
    expect("setup PROVE from code 123456",
           encode_prove(0x1234, setup_key("123456", 0x1234), challenge) ==
           vec("setup prove", "02 00 34 12 05 45 13 4e 24 62 bb 4d cb 21 37 60 44 ae ca 9e"))
    expect("enrolment metadata, issued key",
           decode_enrol_key(vec("enrol issued", "34 12 00 00 a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af"))
           == (0x1234, key))
    for bad in ("12345", "1234567", "12a456", ""):
        try:
            setup_key(bad, 1)
            expect(f"setup code {bad!r} refused", False)
        except HarnessError:
            expect(f"setup code {bad!r} refused", True)

    expect("CRC-8/SMBUS check value", crc8(b"123456789") == 0xF4)

    wire = vec("record", "b1 04 00 00 7f 00 01 00 10 c7 55 69 80 ee 36 00 "
                         "00 00 00 00 00 00 ac 41 00 00 35 42 80 e6 c5 47 "
                         "00 00 19 44 66 66 34 43 66 66 1a 42 35 5e 7a 40 "
                         "07 00 00 00")
    record = decode_record(wire)
    expect("record 1201", record["sequence"] == 1201 and record["valid"] == 0x7F
           and record["time_valid"] and record["epoch_s"] == epoch_ms // 1000 + 3600
           and record["uptime_ms"] == 3600000 and record["boot"] == 7
           and close(record["temperature"], 21.5) and close(record["humidity"], 45.25)
           and close(record["pressure"], 101325) and close(record["co2"], 612)
           and close(record["light"], 180.4, 1e-3) and close(record["noise"], 38.6, 1e-3)
           and close(record["battery"], 3.912), record)
    expect("CSV row matches the USB dump format",
           record_csv_row(record) == ["1201", "7", str(epoch_ms // 1000 + 3600), "3600000",
                                      "127", "21.50", "45.25", "101325", "612", "180.4",
                                      "38.6", "3.912", "", ""], record_csv_row(record))

    header = vec("data header", "01 00 b1 04 00 00 01 00 01 00 00 06")
    data_packet = header + wire
    session = LogSession(1201)
    session.push(data_packet)
    expect("DATA packet CRC over header and record", packet_crc_ok(data_packet))
    expect("DATA packet yields record 1201", [r["sequence"] for r in session.records] == [1201])
    expect("DATA packet builder", build_packet(PKT_DATA, 0, 1201, 1, 0, 1, wire) == data_packet)

    fragment0 = vec("fragment", "03 00 b1 04 00 00 01 00 01 00 00 35 b1 04 00 00 7f 00 01 00")
    expect("FRAGMENT packet builder", build_packet(PKT_FRAGMENT, 0, 1201, 1, 0, 1, wire[:8]) == fragment0)
    fragments = [build_packet(PKT_FRAGMENT, i, 1201, 1, i, 1, wire[i * 8:i * 8 + 8]) for i in range(7)]
    expect("seven fragments at a 20-byte payload",
           len(fragments) == 7 and [len(f) - 12 for f in fragments] == [8] * 6 + [4])
    end = vec("end", "02 01 b2 04 00 00 00 00 00 00 00 ab")
    expect("END packet builder", build_packet(PKT_END, 1, 1202, 0, 0, 0) == end)
    session = LogSession(1201)
    for packet in fragments:
        session.push(packet)
    session.push(build_packet(PKT_END, 7, 1202, 0, 0, 0))
    expect("fragments reassemble record 1201",
           [r["raw"] for r in session.records] == [wire] and session.discarded == 0)
    expect("END sets the cursor", session.ended and session.next_cursor() == 1202)

    session = LogSession(1201)
    for i, packet in enumerate(fragments):
        if i != 3:
            session.push(packet)
    # Fragments 4-6 have no chain to join, so each is discarded, as in the app.
    expect("missing fragment drops the record", session.records == [] and session.discarded == 3,
           f"discarded {session.discarded}")

    session = LogSession(1201)
    corrupt = bytearray(data_packet)
    corrupt[30] ^= 0xFF
    session.push(bytes(corrupt))
    expect("bad CRC discarded and counted",
           session.records == [] and session.crc_failures == 1 and session.discarded == 1)
    expect("cursor without records stays at the request", session.next_cursor() == 1201)

    def wire_for(seq):
        return struct.pack("<I", seq) + wire[4:]

    session = LogSession(10)
    session.push(build_packet(PKT_DATA, 0, 10, 3, 0, 5, wire_for(10) + wire_for(11) + wire_for(13)))
    session.push(build_packet(PKT_DATA, 1, 13, 1, 0, 2, wire_for(13)))
    expect("records need not be consecutive; duplicates counted",
           [r["sequence"] for r in session.records] == [10, 11, 13] and session.duplicates == 1)
    expect("cursor without END is one past the highest", not session.ended and session.next_cursor() == 14)
    gaps, dups, disorder = sequence_problems(session.records)
    expect("gap detection", gaps == [(12, 12)] and dups == 0 and disorder == 0)

    expect("log replaced when newest < cursor - 1",
           log_was_replaced(100, 50) and not log_was_replaced(100, 99)
           and not log_was_replaced(0, 0))

    # The vectors above must still be in the document they came from.
    doc_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                            "protocol", "PROTOCOL.md")
    if os.path.exists(doc_path):
        with open(doc_path) as handle:
            doc = re.sub(r"\s+", " ", handle.read().lower())
        version = re.search(r"protocol version: (\d+)", doc)
        expect("PROTOCOL.md is protocol version 6",
               version is not None and int(version.group(1)) == PROTOCOL_VERSION)
        for name, text in vectors.items():
            expect(f"PROTOCOL.md still contains the {name} vector",
                   re.sub(r"\s+", " ", text.lower()) in doc)
    else:
        print(f"skip PROTOCOL.md cross-check ({doc_path} not found)")

    print(f"\nself-test: {len(failures)} failure(s)" if failures else "\nself-test: all passed")
    return 1 if failures else 0


# ============================================================== BLE transport

OPEN_UNITS = []


def _import_bleak():
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        sys.exit("bleak is not installed: pip install bleak")
    return BleakClient, BleakScanner


def _uuid(short):
    return sig_uuid(short) if short in DIS else quiesco_uuid(short)


class Unit:
    """One connection, with timeouts on every request and notifications fanned
    out to any number of handlers."""

    def __init__(self, device):
        self.device = device
        self.client = None
        self.handlers = {}
        self.dropped = asyncio.Event()

    async def connect(self):
        BleakClient, _ = _import_bleak()
        self.dropped.clear()
        self.handlers = {}
        self.client = BleakClient(self.device, disconnected_callback=lambda _c: self.dropped.set(),
                                  timeout=CONNECT_S)
        OPEN_UNITS.append(self)
        try:
            await asyncio.wait_for(self.client.connect(), CONNECT_S + 5)
        except asyncio.TimeoutError:
            raise HarnessError(f"connecting to {self.device.address} timed out")

    @property
    def connected(self):
        return self.client is not None and self.client.is_connected and not self.dropped.is_set()

    @property
    def mtu(self):
        return self.client.mtu_size

    @property
    def payload(self):
        """ATT payload, clamped the way the unit clamps it (§7.2)."""
        return min(180, max(20, self.client.mtu_size - 3))

    async def read(self, short, timeout=REQUEST_S):
        try:
            return bytes(await asyncio.wait_for(self.client.read_gatt_char(_uuid(short)), timeout))
        except asyncio.TimeoutError:
            raise HarnessError(f"read {short} timed out after {timeout:.0f} s")

    async def write(self, short, data, timeout=REQUEST_S):
        try:
            await asyncio.wait_for(
                self.client.write_gatt_char(_uuid(short), bytes(data), response=True), timeout)
        except asyncio.TimeoutError:
            raise HarnessError(f"write {short} timed out after {timeout:.0f} s")

    async def subscribe(self, short, handler):
        if short not in self.handlers:
            self.handlers[short] = []

            def dispatch(_char, data, short=short):
                for h in list(self.handlers.get(short, [])):
                    h(bytes(data))

            await asyncio.wait_for(self.client.start_notify(_uuid(short), dispatch), REQUEST_S)
        self.handlers[short].append(handler)

    def unsubscribe(self, short, handler):
        with contextlib.suppress(ValueError, KeyError):
            self.handlers[short].remove(handler)

    async def disconnect(self):
        if self.client is not None:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self.client.disconnect(), 10)
        if self in OPEN_UNITS:
            OPEN_UNITS.remove(self)


async def find_unit(args, address=None, timeout=None):
    """First unit advertising the Quiesco service that matches --name/--address."""
    _, BleakScanner = _import_bleak()
    address = (address or args.address or "").lower()

    def matches(device, adv):
        if SERVICE not in [u.lower() for u in adv.service_uuids]:
            return False
        if address and device.address.lower() != address:
            return False
        if args.name and (adv.local_name or device.name) != args.name:
            return False
        return True

    device = await BleakScanner.find_device_by_filter(
        matches, timeout=timeout or args.scan_time, service_uuids=[SERVICE])
    if device is None:
        what = address or (f"named {args.name!r}" if args.name else "advertising the Quiesco service")
        raise HarnessError(f"no unit {what} found in {timeout or args.scan_time:.0f} s "
                           "(is the companion app connected to it? is BLE policy 'plugged "
                           "in' and the unit on battery?)")
    return device


def load_keys(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path) as handle:
            return json.load(handle)
    except (OSError, ValueError) as e:
        raise HarnessError(f"cannot read key file {path}: {e}")


def save_key(path, serial, key_id, key):
    keys = load_keys(path)
    keys[serial] = {"keyId": key_id, "key": key.hex()}
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as handle:
        json.dump(keys, handle, indent=2)
    os.replace(tmp, path)


def stored_key(args, serial):
    entry = load_keys(args.key_file).get(serial)
    if not entry:
        return None
    try:
        return int(entry["keyId"]), bytes.fromhex(entry["key"])
    except (KeyError, ValueError, TypeError):
        raise HarnessError(f"key file entry for {serial} is malformed")


async def read_identity(unit):
    info = decode_device_info(await unit.read(DEVICE_INFO))
    serial = (await unit.read("2A25")).decode(errors="replace")
    firmware = (await unit.read("2A26")).decode(errors="replace")
    return info, serial, firmware


def require_protocol(info):
    if info["protocol"] != PROTOCOL_VERSION:
        raise HarnessError(f"unit speaks protocol {info['protocol']}, this harness "
                           f"{PROTOCOL_VERSION}; refusing to write (§1)")
    if not has_cap(info, CAP_AUTH):
        raise HarnessError("unit lacks capability bit 10 (app-layer auth)")


async def fresh_challenge(unit):
    deadline = time.monotonic() + AUTH_WAIT_S
    while True:
        state = decode_auth_state(await unit.read(AUTH))
        if state["authenticated"] or any(state["challenge"]):
            return state
        if time.monotonic() > deadline:
            raise HarnessError("the unit published no challenge (HCI LE Rand failing?)")
        await asyncio.sleep(AUTH_POLL_S)


async def prove(unit, key_id, key):
    """True when the unit accepted the proof, False when it refused it (§6.15)."""
    state = await fresh_challenge(unit)
    if state["authenticated"]:
        return True
    challenge = state["challenge"]
    message = encode_prove(key_id, key, challenge)
    await unit.write(AUTH, message)
    deadline = time.monotonic() + AUTH_WAIT_S
    while time.monotonic() < deadline:
        await asyncio.sleep(AUTH_POLL_S)
        raw = await unit.read(AUTH)
        if raw[:len(message)] == message:
            continue  # ArduinoBLE echoes the write until the main loop answers
        state = decode_auth_state(raw)
        if state["authenticated"]:
            return True
        if any(state["challenge"]) and state["challenge"] != challenge:
            return False
    raise HarnessError("the unit did not answer the proof")


async def authenticate(unit, args, serial):
    key = stored_key(args, serial)
    if key is None:
        raise HarnessError(f"no key for unit {serial} in {args.key_file}; run 'enrol' with "
                           "the unit on USB power first")
    # One refusal can be the unit rotating its challenge; the app retries once too.
    for _ in range(2):
        if await prove(unit, *key):
            return key
    raise HarnessError(f"the unit refused key id {key[0]:#06x} twice (factory reset or "
                       "evicted?); run 'enrol' again")


async def open_ready(args, device=None):
    """§2 connect sequence: connect, identity, auth, clock. Returns (unit, info, serial)."""
    device = device or await find_unit(args)
    unit = Unit(device)
    await unit.connect()
    info, serial, _ = await read_identity(unit)
    require_protocol(info)
    await authenticate(unit, args, serial)
    await unit.write(EPOCH, encode_epoch(time.time() * 1000))
    return unit, info, serial


# ============================================================ log download

async def download_session(unit, start, max_records, stop_after=None, progress=None):
    """One START → END session. With stop_after, returns as soon as that many
    records arrived (the caller cuts the link)."""
    session = LogSession(start)
    packets = asyncio.Queue()
    inactive = asyncio.Event()
    newest = {"value": None}

    def on_packet(data):
        packets.put_nowait(data)

    def on_status(data):
        with contextlib.suppress(HarnessError):
            status = decode_status(data)
            newest["value"] = status["newest_sequence"]
            if not status["download_active"]:
                inactive.set()

    await unit.subscribe(LOG_DATA, on_packet)  # §7.1: subscribe before START
    await unit.subscribe(STATUS, on_status)
    started = time.monotonic()
    first_packet = None
    try:
        await unit.write(SYNC_CONTROL, encode_log_start(start, max_records, unit.payload))
        last = time.monotonic()
        while not session.ended:
            if unit.dropped.is_set():
                raise HarnessError("the unit disconnected during the download")
            try:
                packet = await asyncio.wait_for(packets.get(), 0.5)
            except asyncio.TimeoutError:
                if inactive.is_set() and session.packets and packets.empty():
                    break  # streaming ended without END (§7.1 step 4)
                limit = LOG_STALL_S if session.packets else LOG_START_S
                if time.monotonic() - last > limit:
                    with contextlib.suppress(Exception):
                        await unit.write(SYNC_CONTROL, encode_log_abort())
                    raise HarnessError(f"no log packet for {limit:.0f} s")
                continue
            last = time.monotonic()
            if first_packet is None:
                first_packet = last - started
            before = len(session.records)
            session.push(packet)
            if progress and len(session.records) // 1000 != before // 1000:
                progress(len(session.records))
            if stop_after and len(session.records) >= stop_after:
                break
    finally:
        unit.unsubscribe(LOG_DATA, on_packet)
        unit.unsubscribe(STATUS, on_status)
    session.seconds = time.monotonic() - started
    session.first_packet_s = first_packet
    session.newest = newest["value"]
    return session


async def download_all(unit, start=0, batch=2000, quiet=False):
    """Batches like the app (link.ts runSync). Returns (records, totals dict)."""
    status = decode_status(await unit.read(STATUS))
    cursor = 0 if log_was_replaced(start, status["newest_sequence"]) else start
    newest = status["newest_sequence"]
    records = []
    totals = {"packets": 0, "bytes": 0, "crc_failures": 0, "discarded": 0, "duplicates": 0,
              "counter_skips": 0, "sessions": 0, "seconds": 0.0, "ended": True,
              "first_packet_s": []}

    def progress(n):
        if not quiet:
            print(f"  ... {len(records) + n} records", file=sys.stderr, flush=True)

    while True:
        session = await download_session(unit, cursor, batch, progress=progress)
        records += session.records
        totals["sessions"] += 1
        for k in ("packets", "bytes", "crc_failures", "discarded", "duplicates", "counter_skips"):
            totals[k] += getattr(session, k)
        totals["seconds"] += session.seconds
        if session.first_packet_s is not None:
            totals["first_packet_s"].append(session.first_packet_s)
        newest = session.newest if session.newest is not None else newest
        nxt = session.next_cursor()
        if not session.ended:
            totals["ended"] = False
        if not session.ended or batch == 0 or len(session.records) < batch or nxt > newest:
            totals["cursor"] = nxt
            return records, totals
        cursor = nxt


# ================================================================== commands

async def cmd_scan(args):
    _, BleakScanner = _import_bleak()
    found = await BleakScanner.discover(timeout=args.scan_time, return_adv=True,
                                        service_uuids=[SERVICE])
    units = [(d, a) for d, a in found.values()
             if SERVICE in [u.lower() for u in a.service_uuids]]
    if not units:
        print("no Quiesco unit advertising")
        return 1
    for device, adv in sorted(units, key=lambda x: -x[1].rssi):
        print(f"{adv.rssi:4d} dBm  {adv.local_name or device.name or '?':16}  {device.address}")
    return 0


def print_fields(title, fields):
    print(f"== {title}")
    for k, v in fields.items():
        if k == "raw":
            continue
        if k == "capabilities":
            names = [n for bit, n in enumerate(CAPABILITY_NAMES) if v >> bit & 1]
            v = f"{v:#x} ({', '.join(names)})"
        elif k == "challenge":
            v = hexs(v)
        elif isinstance(v, float):
            v = f"{v:.3f}"
        print(f"  {k:24} {v}")


async def cmd_info(args):
    device = await find_unit(args)
    unit = Unit(device)
    await unit.connect()
    try:
        print(f"connected to {device.address}, MTU {unit.mtu} (ATT payload {unit.mtu - 3})")
        dis = {}
        for short, label in DIS.items():
            try:
                dis[label] = (await unit.read(short)).decode(errors="replace")
            except Exception as e:  # noqa: BLE001 - report every characteristic
                dis[label] = f"<{e}>"
        print_fields("Device Information Service", dis)
        info = decode_device_info(await unit.read(DEVICE_INFO))
        print_fields("Device info (000C)", info)
        print_fields("Auth (0010)", decode_auth_state(await unit.read(AUTH)))
        key_id, key = decode_enrol_key(await unit.read(ENROL_KEY))
        print_fields("Enrolment metadata (0011)",
                     {"pending_key_id": f"{key_id:#06x}", "key_present": any(key)})
        has_key = stored_key(args, dis.get("serial", "")) is not None
        print(f"\nkey for this serial in {args.key_file}: {'yes' if has_key else 'no'}")
        if has_cap(info, CAP_SLEEP_WINDOW):
            if has_key:
                await authenticate(unit, args, dis["serial"])
                window = decode_sleep_window(await unit.read(SLEEP_WINDOW))
                print_fields("Sleep window (0012)", window)
            else:
                print("sleep window (0012): needs a key (protected)")
        if info["protocol"] != PROTOCOL_VERSION:
            print(f"WARNING: protocol {info['protocol']}, harness speaks {PROTOCOL_VERSION}")
    finally:
        await unit.disconnect()
    return 0


async def cmd_enrol(args):
    device = await find_unit(args)
    unit = Unit(device)
    await unit.connect()
    try:
        info, serial, firmware = await read_identity(unit)
        require_protocol(info)
        print(f"unit {serial}, firmware {firmware}")
        state = await fresh_challenge(unit)
        if state["authenticated"]:
            raise HarnessError("this connection is already authenticated?")
        if not state["enrolment_open"]:
            raise HarnessError("enrolment is closed: put the unit on USB power and retry")
        if stored_key(args, serial):
            print(f"note: {args.key_file} already holds a key for {serial}; it will be replaced")
        await unit.write(AUTH, encode_enrol())
        key_id = 0
        deadline = time.monotonic() + AUTH_WAIT_S
        while not key_id and time.monotonic() < deadline:
            await asyncio.sleep(AUTH_POLL_S)
            raw = await unit.read(ENROL_KEY)
            key_id, key = decode_enrol_key(raw)
            if any(raw[2:]):
                raise HarnessError(f"0011 holds more than a key id while pending: {hexs(raw)}")
        if not key_id:
            raise HarnessError("the unit did not open a setup (on USB? ENROL refused after 5 "
                               "failed proofs on this connection?)")
        print(f"setup pending, key id {key_id:#06x}; the panel shows the code (expires in 180 s)")
        for attempt in range(3):
            entered = await asyncio.to_thread(input, "six-digit code on the panel: ")
            code = re.sub(r"\s", "", entered)
            if not re.fullmatch(r"\d{6}", code):
                print("  six digits, please")
                continue
            if await prove(unit, key_id, setup_key(code, key_id)):
                break
            print("  refused (each wrong code uses one of 5 attempts per connection)")
        else:
            raise HarnessError("no code accepted; reconnect for a new code")
        deadline = time.monotonic() + ISSUED_KEY_WAIT_S
        while time.monotonic() < deadline:
            issued_id, issued = decode_enrol_key(await unit.read(ENROL_KEY))
            if issued_id == key_id and any(issued):
                save_key(args.key_file, serial, key_id, issued)
                print(f"enrolled: key id {key_id:#06x} saved to {args.key_file}")
                return 0
            await asyncio.sleep(AUTH_POLL_S)
        raise HarnessError("the code was accepted but 0011 never held the phone key; the "
                           "unit stored a key we lack - enrol again")
    finally:
        await unit.disconnect()


async def cmd_download(args):
    unit, info, serial = await open_ready(args)
    try:
        if not has_cap(info, CAP_LOG):
            raise HarnessError("unit lacks capability bit 4 (log download)")
        print(f"unit {serial}: downloading from {args.start}, MTU {unit.mtu}, "
              f"payload {unit.payload}", file=sys.stderr)
        records, totals = await download_all(unit, args.start, args.batch)
    finally:
        await unit.disconnect()
    with open(args.out, "w", newline="") as handle:
        last = records[-1]["sequence"] if records else 0
        handle.write(f"# begin quiesco-log v1 source=ble serial={serial} last={last}\n")
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(CSV_FIELDS)
        for r in records:
            writer.writerow(record_csv_row(r))
        handle.write(f"# end {'' if totals['ended'] else 'incomplete '}records={len(records)}\n")
    rate = len(records) / totals["seconds"] if totals["seconds"] else 0
    print(f"{len(records)} records, {totals['bytes']} bytes in {totals['seconds']:.1f} s "
          f"({rate:.0f} records/s), {totals['discarded']} packets discarded "
          f"({totals['crc_failures']} CRC), next cursor {totals['cursor']} -> {args.out}")
    return 0 if totals["ended"] else 1


# ================================================================== run suite

class Report:
    def __init__(self):
        self.results = []

    def add(self, name, status, detail=""):
        self.results.append((name, status))
        print(f"{status:4} {name}" + (f" — {detail}" if detail else ""), flush=True)

    def summary(self):
        counts = {s: sum(1 for _, r in self.results if r == s)
                  for s in ("PASS", "FAIL", "WARN", "SKIP")}
        print("\n== Summary: " + ", ".join(f"{n} {s}" for s, n in counts.items()))
        for name, status in self.results:
            if status == "FAIL":
                print(f"  FAIL {name}")
        return 1 if counts["FAIL"] else 0


def describe(short, data):
    decoders = {INTERVAL: decode_interval, CORE_CONFIG: decode_core_config,
                READING: decode_reading, STATUS: decode_status, EPOCH: decode_epoch,
                OFFSETS: decode_offsets, CAL_STATE: decode_calibration_state,
                DIAGNOSTICS: decode_diagnostics, DEVICE_INFO: decode_device_info,
                AUTH: decode_auth_state, ENROL_KEY: decode_enrol_key,
                SLEEP_WINDOW: lambda d: decode_sleep_window(d)["summary"]}
    if short == SCREEN:
        return {"screen": data[0] if data else None}
    if short == NAME or short in DIS:
        return {"text": data.decode(errors="replace")}
    value = decoders[short](data)
    if short == AUTH:
        value = dict(value, challenge=hexs(value["challenge"]))
    if short == ENROL_KEY:
        value = {"key_id": value[0], "key_present": any(value[1])}
    return value


class Suite:
    def __init__(self, args):
        self.args = args
        self.report = Report()
        self.unit = None
        self.device = None
        self.serial = None
        self.info = None
        self.baseline = None
        self.notes = []  # (monotonic time, characteristic, bytes)
        self.auth_at = None
        self.reference = None  # full download, for the resume test

    def check(self, name, ok, detail="", warn=False):
        self.report.add(name, "PASS" if ok else ("WARN" if warn else "FAIL"), detail)
        return ok

    def expected(self):
        """§4 characteristics this unit's capabilities promise."""
        return {s: v for s, v in CHARACTERISTICS.items()
                if s not in CHARACTERISTIC_CAPS or has_cap(self.info, CHARACTERISTIC_CAPS[s])}

    def protected_reads(self):
        extra = [SLEEP_WINDOW] if has_cap(self.info, CAP_SLEEP_WINDOW) else []
        return PROTECTED_READS + extra

    # -------------------------------------------------------------- plumbing

    async def reconnect(self, authenticate_now=True):
        if self.unit:
            await self.unit.disconnect()
            await asyncio.sleep(1.0)
        self.device = await find_unit(self.args, address=self.device.address if self.device else None)
        self.unit = Unit(self.device)
        await self.unit.connect()
        if authenticate_now:
            await authenticate(self.unit, self.args, self.serial)
            await self.unit.write(EPOCH, encode_epoch(time.time() * 1000))
            await self.unit.subscribe(READING, self.on_note(READING))
            await self.unit.subscribe(STATUS, self.on_note(STATUS))

    def on_note(self, short):
        return lambda data: self.notes.append((time.monotonic(), short, data))

    async def snapshot(self):
        """What a malformed write must not change."""
        snap = {}
        shorts = [INTERVAL, CORE_CONFIG, EPOCH, SCREEN, OFFSETS, NAME]
        if has_cap(self.info, CAP_SLEEP_WINDOW):
            shorts.append(SLEEP_WINDOW)
        for short in shorts:
            snap[short] = await self.unit.read(short)
        status = decode_status(await self.unit.read(STATUS))
        snap["status"] = (status["download_active"], status["erasing"], status["frc_state"])
        snap["authenticated"] = decode_auth_state(await self.unit.read(AUTH))["authenticated"]
        return snap

    async def settled(self, expected):
        """Poll until the unit shows `expected` again (it reverts a rejected
        write from its main loop, §5); returns (ok, last snapshot)."""
        deadline = time.monotonic() + SETTLE_S
        while True:
            await asyncio.sleep(0.3)
            snap = await self.snapshot()
            if snap == expected or time.monotonic() > deadline:
                return snap == expected, snap

    # ---------------------------------------------------------------- checks

    async def run(self):
        args = self.args
        self.device = await find_unit(args)
        self.unit = Unit(self.device)
        await self.unit.connect()
        print(f"connected to {self.device.address}")
        self.info, self.serial, firmware = await read_identity(self.unit)
        print(f"unit {self.serial}, firmware {firmware}, protocol {self.info['protocol']}\n")
        if not self.check("M3 protocol version is 6", self.info["protocol"] == PROTOCOL_VERSION,
                          f"unit says {self.info['protocol']}"):
            return
        if stored_key(args, self.serial) is None:
            raise HarnessError(f"no key for {self.serial} in {args.key_file}; run 'enrol' first")

        await self.m2_discovery()
        await self.m5_before_auth()
        await self.m5_auth()
        await self.m2_read_all()
        await self.m4_clock()
        if "robustness" not in args.skip:
            await self.m2_robustness()
        if "rapid" not in args.skip:
            await self.m2_rapid()
        await self.m4_offsets()
        if "sleep-window" not in args.skip:
            await self.m4_sleep_window()
        if "log" not in args.skip:
            await self.m4_log()
        if "erase-refusal" not in args.skip:
            await self.m4_erase_refusal()
        await self.m2_notify_measurement()
        await self.m4_clock_status()
        if "unauth-write" not in args.skip:
            await self.m5_unauth_write()
        if "log" not in args.skip and "resume" not in args.skip:
            await self.m4_resume()
        if "reconnect" not in args.skip:
            await self.m5_reconnect()

    async def m2_discovery(self):
        services = self.unit.client.services
        found = {}
        for service in services:
            for char in service.characteristics:
                found[char.uuid.lower()] = (service.uuid.lower(), set(char.properties))
        missing, wrong = [], []
        expected = self.expected()
        for short, (label, props) in expected.items():
            entry = found.get(quiesco_uuid(short))
            if entry is None or entry[0] != SERVICE:
                missing.append(f"{short} {label}")
            elif not props <= entry[1]:
                wrong.append(f"{short} has {sorted(entry[1])}, wants {sorted(props)}")
        for short, label in DIS.items():
            entry = found.get(sig_uuid(short))
            if entry is None or entry[0] != DIS_SERVICE:
                missing.append(f"DIS {short} {label}")
        self.check("M2 discover: every characteristic of §4 present", not missing,
                   f"missing: {', '.join(missing)}" if missing else
                   f"{len(expected)} Quiesco + {len(DIS)} DIS")
        self.check("M2 discover: properties match §4", not wrong, "; ".join(wrong))
        extra = [f"{s} {sorted(found[quiesco_uuid(s)][1] - p)}"
                 for s, (_, p) in expected.items()
                 if quiesco_uuid(s) in found and found[quiesco_uuid(s)][1] - p
                 - {"write-without-response"}]
        if extra:
            self.report.add("M2 discover: no extra properties", "WARN", "; ".join(extra))
        mtu = self.unit.mtu
        self.check("M2 negotiated MTU", mtu >= 23,
                   f"MTU {mtu}, ATT payload {mtu - 3}, log packets sized for {self.unit.payload} "
                   f"({'3 records' if self.unit.payload >= 168 else '2 records' if self.unit.payload >= 116 else '1 record' if self.unit.payload >= 64 else 'fragments'})")

    async def m5_before_auth(self):
        state = await fresh_challenge(self.unit)
        self.check("M5 new connection starts unauthenticated", not state["authenticated"])
        self.check("M5 challenge published before auth", any(state["challenge"]))
        nonzero = []
        for short in self.protected_reads():
            data = await self.unit.read(short)
            if any(data):
                nonzero.append(f"{short}={hexs(data)}")
        self.check("M5 protected characteristics read as zeros before auth", not nonzero,
                   "; ".join(nonzero) or f"{len(self.protected_reads())} read blank")
        dis_ok = all([await self.unit.read(s) for s in DIS])
        self.check("M5 DIS, 000C, 0010, 0011 readable before auth",
                   dis_ok and self.info["protocol"] == PROTOCOL_VERSION)
        await self.unit.subscribe(READING, self.on_note(READING))
        await self.unit.subscribe(STATUS, self.on_note(STATUS))
        await asyncio.sleep(self.args.preauth_wait)
        early = [n for n in self.notes]
        self.check(f"M5 no notification before auth ({self.args.preauth_wait:.0f} s subscribed)",
                   not early, f"{len(early)} arrived: " +
                   ", ".join(f"{s}={hexs(d)}" for _, s, d in early[:3]))

    async def m5_auth(self):
        key_id, key = stored_key(self.args, self.serial)
        wrong = await prove(self.unit, key_id, secrets.token_bytes(16))
        self.check("M5 wrong key refused", not wrong)
        if wrong:
            raise HarnessError("a random key was accepted; stopping")
        state = decode_auth_state(await self.unit.read(AUTH))
        self.check("M5 refusal rotates the challenge, connection still blank",
                   not state["authenticated"] and any(state["challenge"])
                   and not any(await self.unit.read(CORE_CONFIG)))
        before = len(self.notes)
        ok = await prove(self.unit, key_id, key)
        self.auth_at = time.monotonic()
        self.check("M5 stored key accepted", ok, f"key id {key_id:#06x}")
        if not ok:
            raise HarnessError("the stored key was refused; run 'enrol' again")
        await asyncio.sleep(3.0)
        burst = {s for _, s, _ in self.notes[before:]}
        self.check("M5 reading and status notified at once on auth (§3)",
                   {READING, STATUS} <= burst, f"got {sorted(burst) or 'none'} within 3 s")
        info = decode_device_info(await self.unit.read(DEVICE_INFO))
        self.check("M5 device info reports a phone enrolled", info["enrolled"])

    async def m2_read_all(self):
        problems = []
        for short in list(DIS) + [s for s, (_, p) in self.expected().items() if "read" in p]:
            try:
                data = await self.unit.read(short)
                value = describe(short, data)
                print(f"     {short} {CHARACTERISTICS.get(short, (DIS.get(short),))[0]}: {value}")
            except Exception as e:  # noqa: BLE001
                problems.append(f"{short}: {e}")
        self.check("M2 every readable characteristic reads and decodes", not problems,
                   "; ".join(problems))
        config = decode_core_config(await self.unit.read(CORE_CONFIG))
        self.config = config
        interval = decode_interval(await self.unit.read(INTERVAL))
        screen = (await self.unit.read(SCREEN))[0]
        self.check("M2 0001 and 0006 agree with core config",
                   interval == config["interval_s"] and screen == config["screen"],
                   f"interval {interval}/{config['interval_s']}, screen {screen}/{config['screen']}")
        self.check("M2 core config values in range",
                   config["interval_s"] in INTERVALS_S and 1 <= config["full_refresh_every"] <= 1000
                   and config["screen"] <= 2 and config["reserved"] == 0, str(config))

    async def m4_clock(self):
        now_ms = int(time.time() * 1000)
        await self.unit.write(EPOCH, encode_epoch(now_ms))
        await asyncio.sleep(0.5)
        echoed = decode_epoch(await self.unit.read(EPOCH))
        self.check("M4 clock sync: epoch reads back the value written", echoed == now_ms,
                   f"wrote {now_ms}, read {echoed}")
        self.clock_synced_now = decode_status(await self.unit.read(STATUS))["clock_synced"]

    async def m4_clock_status(self):
        if self.clock_synced_now:
            self.report.add("M4 status clockSynced", "PASS", "set right after the write")
            return
        status = decode_status(await self.unit.read(STATUS))
        later = [s for t, c, s in self.notes if c == STATUS and t > self.auth_at + 3]
        if status["clock_synced"]:
            self.report.add("M4 status clockSynced", "PASS",
                            "set after the next status publish (not at the epoch write)")
        elif later:
            self.report.add("M4 status clockSynced", "FAIL",
                            "a measurement's status arrived without bit 0")
        else:
            self.report.add("M4 status clockSynced", "WARN",
                            "not set yet; status only republishes at a measurement")

    async def m2_robustness(self):
        if not self.unit.connected:
            await self.reconnect()
        self.baseline = await self.snapshot()
        config = bytearray(await self.unit.read(CORE_CONFIG))
        name = await self.unit.read(NAME)
        window = (await self.unit.read(SLEEP_WINDOW)
                  if has_cap(self.info, CAP_SLEEP_WINDOW) else None)

        def cfg(**changes):
            c = decode_core_config(bytes(config))
            c.update(changes)
            return struct.pack("<IIIBBBB", c["version"], c["interval_s"], c["full_refresh_every"],
                               c["ble_always_available"], c["screen"], c["temperature_unit"],
                               c["reserved"])

        long_interval = 3600  # invalid but long: never a short interval, even if accepted
        nan = struct.pack("<f", math.nan)
        inf = struct.pack("<f", math.inf)
        # (characteristic, label, bytes). Never: opcode 1 to 000B; ENROL; the
        # confirmation c7 fa anywhere in 000D.
        cases = [
            (INTERVAL, "too short", struct.pack("<I", 1800)[:3]),
            (INTERVAL, "too long", struct.pack("<I", 1800) + b"\0"),
            (INTERVAL, "not an allowed interval", struct.pack("<I", long_interval)),
            (INTERVAL, "0xffffffff", b"\xff\xff\xff\xff"),
            (CORE_CONFIG, "too short", bytes(config[:15])),
            (CORE_CONFIG, "too long", bytes(config) + b"\0"),
            (CORE_CONFIG, "wrong config version", cfg(version=decode_core_config(bytes(config))["version"] + 1)),
            (CORE_CONFIG, "bad interval", cfg(interval_s=long_interval)),
            (CORE_CONFIG, "full refresh 0", cfg(full_refresh_every=0)),
            (CORE_CONFIG, "full refresh 1001", cfg(full_refresh_every=1001)),
            (CORE_CONFIG, "BLE policy 2", cfg(ble_always_available=2)),
            (CORE_CONFIG, "screen 3", cfg(screen=3)),
            (CORE_CONFIG, "temperature unit 2", cfg(temperature_unit=2)),
            (CORE_CONFIG, "reserved byte set", cfg(reserved=1)),
            (EPOCH, "too short", encode_epoch(time.time() * 1000)[:7]),
            (EPOCH, "too long", encode_epoch(time.time() * 1000) + b"\0"),
            (EPOCH, "zero", bytes(8)),
            (EPOCH, "2^32 s", struct.pack("<Q", 2 ** 32 * 1000)),
            (SCREEN, "empty", b""),
            (SCREEN, "too long", bytes([config[13], 0])),
            (SCREEN, "screen 3", b"\x03"),
            (SCREEN, "screen 255", b"\xff"),
            (OFFSETS, "too short", bytes(11)),
            (OFFSETS, "too long", bytes(13)),
            (OFFSETS, "NaN", nan + bytes(8)),
            (OFFSETS, "infinity", bytes(4) + inf + bytes(4)),
            (OFFSETS, "noise +25 dB", struct.pack("<fff", 25.0, 0, 0)),
            (OFFSETS, "temperature -9 C", struct.pack("<fff", 0, -9.0, 0)),
            (NAME, "empty", b""),
            (NAME, "17 bytes", b"Q" * 17),
            (NAME, "control character", b"Quiesco\nX"),
            (NAME, "embedded NUL", b"Quies\0co"),
            (NAME, "DEL", b"Quiesco\x7f"),
            (SYNC_CONTROL, "too short", encode_log_start(0, 1, 20)[:9]),
            (SYNC_CONTROL, "too long", encode_log_abort() + b"\0"),
            (SYNC_CONTROL, "opcode 0", bytes(10)),
            (SYNC_CONTROL, "opcode 3", b"\x03" + bytes(9)),
            (SYNC_CONTROL, "opcode 255", b"\xff" + bytes(9)),
            (SYNC_CONTROL, "ABORT while idle", encode_log_abort()),
            (CAL_CONTROL, "too short", b"\x00\x00\xa4"),
            (CAL_CONTROL, "too long", b"\x00\x00\xa4\x01\x00"),
            (CAL_CONTROL, "opcode 0", b"\x00\x00\xa4\x01"),
            (CAL_CONTROL, "opcode 2", b"\x02\x00\xa4\x01"),
            (CAL_CONTROL, "opcode 255", b"\xff\x00\xa4\x01"),
            (DEVICE_CONTROL, "too short", b"\x01\x00\x00"),
            (DEVICE_CONTROL, "too long", b"\x01\x00\x00\x00\x00"),
            (DEVICE_CONTROL, "bad confirmation 0xFAC6", b"\x01\x00\xc6\xfa"),
            (DEVICE_CONTROL, "bad confirmation 0", b"\x01\x00\x00\x00"),
            (DEVICE_CONTROL, "reserved flag bits", b"\x01\x02\x00\x00"),
            (DEVICE_CONTROL, "opcode 0", b"\x00\x00\x00\x00"),
            (DEVICE_CONTROL, "opcode 255", b"\xff\x00\x00\x00"),
            (DEVICE_CONTROL, "erase, bad confirmation", b"\x02\x00\xc6\xfa" + bytes(4)),
            (DEVICE_CONTROL, "erase, 9 bytes, no confirmation", b"\x02" + bytes(8)),
            (AUTH, "1 byte", b"\x02"),
            (AUTH, "ENROL reserved byte set", b"\x01\x01"),
            (AUTH, "ENROL too long", b"\x01\x00\x00"),
            (AUTH, "opcode 3", b"\x03\x00"),
            (AUTH, "PROVE too short", b"\x02\x00" + bytes(17)),
            (AUTH, "PROVE too long", b"\x02\x00" + bytes(19)),
            (DEVICE_INFO, "write to read-only", bytes(20)),
            (ENROL_KEY, "write to read-only", bytes(20)),
            (READING, "write to read-only", bytes(20)),
        ]
        if has_cap(self.info, CAP_SLEEP_WINDOW):
            good = encode_sleep_window(False, 0, (1410, 420))
            cases += [
                (SLEEP_WINDOW, "too short", good[:11]),
                (SLEEP_WINDOW, "too long", good + b"\0"),
                (SLEEP_WINDOW, "reserved flag bit", encode_sleep_window(False, 0, (1410, 420), flags=2)),
                (SLEEP_WINDOW, "reserved byte", good[:1] + b"\x01" + good[2:]),
                (SLEEP_WINDOW, "offset -721", encode_sleep_window(False, -721, (1410, 420))),
                (SLEEP_WINDOW, "bedtime 1440", encode_sleep_window(False, 0, (1440, 420))),
                (SLEEP_WINDOW, "one weekend 0xFFFF", encode_sleep_window(False, 0, (1410, 420),
                                                                          (60, NO_WEEKEND))),
            ]
        if not has_cap(self.info, CAP_RENAME):
            cases = [c for c in cases if c[0] != NAME]
        failures, refused = [], 0
        info_before = await self.unit.read(DEVICE_INFO)
        for short, label, data in cases:
            assert not (short == DEVICE_CONTROL and b"\xc7\xfa" in data)
            assert not (short == CAL_CONTROL and data[:1] == b"\x01")
            try:
                await self.unit.write(short, data)
            except HarnessError as e:
                failures.append(f"{short} {label}: {e}")
                if not self.unit.connected:
                    break
                continue
            except Exception:  # noqa: BLE001 - an ATT error is an acceptable refusal
                refused += 1
            if not self.unit.connected:
                failures.append(f"{short} {label}: link dropped")
                break
            try:
                ok, snap = await self.settled(self.baseline)
            except HarnessError as e:
                failures.append(f"{short} {label}: unit stopped answering ({e})")
                break
            if not ok:
                diff = [k for k in self.baseline if self.baseline[k] != snap[k]]
                failures.append(f"{short} {label}: changed {diff}")
                await self.restore(config, name, window)
                self.baseline = await self.snapshot()
        info_after = await self.unit.read(DEVICE_INFO)
        self.check(f"M2 robustness: {len(cases)} malformed writes change nothing, link survives",
                   not failures, "; ".join(failures[:6]) or
                   f"{refused} refused by the stack with an ATT error, the rest ignored")
        self.check("M2 robustness: read-only device info unchanged",
                   info_after[:10] == info_before[:10])
        if not self.unit.connected:
            print("     link lost: reconnecting")
            await self.reconnect()

    async def restore(self, config, name, window=None):
        with contextlib.suppress(Exception):
            await self.unit.write(CORE_CONFIG, bytes(config))
            await self.unit.write(NAME, name)
            if window is not None:
                await self.unit.write(SLEEP_WINDOW, window)
            await self.unit.write(EPOCH, encode_epoch(time.time() * 1000))
            await asyncio.sleep(1.0)

    async def write_confirmed(self, short, data, attempts=8):
        await self.unit.write(short, data)
        for _ in range(attempts):
            await asyncio.sleep(0.5)
            if await self.unit.read(short) == data:
                return True
        return False

    async def m2_rapid(self):
        if not has_cap(self.info, CAP_RENAME):
            self.report.add("M2 rapid renames", "SKIP", "no rename capability")
        else:
            original = await self.unit.read(NAME)
            started = time.monotonic()
            names = [f"QTest rapid {i}".encode() for i in range(10)]
            for n in names:
                await self.unit.write(NAME, n)
            sent = time.monotonic() - started
            last_wins = False
            for _ in range(16):
                await asyncio.sleep(0.5)
                if await self.unit.read(NAME) == names[-1]:
                    last_wins = True
                    break
            restored = await self.write_confirmed(NAME, original)
            self.check("M2 rapid: 10 renames, last write wins", last_wins,
                       f"10 writes acknowledged in {sent:.2f} s")
            self.check("M2 rapid: original name restored", restored,
                       original.decode(errors="replace"))
        screen = await self.unit.read(SCREEN)
        before = sum(1 for _, s, _ in self.notes if s == READING)
        for _ in range(10):
            await self.unit.write(SCREEN, screen)
        await asyncio.sleep(4.0)
        redraws = sum(1 for _, s, _ in self.notes if s == READING) - before
        self.check("M2 rapid: same screen written 10x, unchanged",
                   await self.unit.read(SCREEN) == screen, f"screen {screen[0]}")
        self.check("M2 rapid: unchanged screen causes no redraw", redraws == 0,
                   f"{redraws} reading notification(s) followed (a measurement can coincide)",
                   warn=True)
        ok, snap = await self.settled(self.baseline or await self.snapshot())
        self.check("M2 rapid: state as before, link alive", ok and self.unit.connected)

    async def m4_offsets(self):
        offsets = decode_offsets(await self.unit.read(OFFSETS)) if has_cap(self.info, CAP_OFFSETS) else None
        cal = decode_calibration_state(await self.unit.read(CAL_STATE)) if has_cap(self.info, CAP_CAL_STATE) else None
        print(f"     offsets: {offsets}\n     calibration state: {cal}")
        sane = offsets is None or (abs(offsets["noise_db"]) <= 24 and abs(offsets["temperature_c"]) <= 8
                                   and abs(offsets["humidity_pct"]) <= 20)
        self.check("M4 offsets and calibration state read (no writes)", sane,
                   "offsets out of the §6.7 range" if not sane else
                   f"ASC off: {cal['asc_off'] if cal else '?'}")

    async def m4_sleep_window(self):
        if not has_cap(self.info, CAP_SLEEP_WINDOW):
            self.report.add("M4 sleep window write and read back", "SKIP", "no capability bit 14")
            return
        if not self.unit.connected:
            await self.reconnect()
        original = await self.unit.read(SLEEP_WINDOW)
        self.check("M4 sleep window reads and is valid", sleep_window_valid(original),
                   decode_sleep_window(original)["summary"] if len(original) == 12 else hexs(original))
        # A window unlike any real one, so the read-back cannot pass by accident;
        # follow-app stays as the user had it so the panel's verdicts are unchanged.
        follow = bool(original[:1] and original[0] & 1)
        local = int(datetime.datetime.now().astimezone().utcoffset().total_seconds() // 60)
        test = encode_sleep_window(follow, local, (1395, 405), (75, 615))
        if test == original:
            test = encode_sleep_window(follow, local, (1380, 390), (90, 630))
        applied = await self.write_confirmed(SLEEP_WINDOW, test)
        self.check("M4 sleep window write reads back", applied, decode_sleep_window(test)["summary"])
        bad = encode_sleep_window(follow, local, (1395, 405), (75, NO_WEEKEND))
        await self.unit.write(SLEEP_WINDOW, bad)
        await asyncio.sleep(1.5)
        after_bad = await self.unit.read(SLEEP_WINDOW)
        self.check("M4 sleep window invalid write rejected, value unchanged", after_bad == test,
                   "only one of the weekend pair 0xFFFF" if after_bad == test else hexs(after_bad))
        restored = await self.write_confirmed(SLEEP_WINDOW, original)
        self.check("M4 sleep window original restored", restored, hexs(original))

    async def m4_erase_refusal(self):
        """Opcode 2 with up-to = newest - 1 must be refused. Never an erase that
        could pass: the owner's log is real (§9.1)."""
        if not has_cap(self.info, CAP_ERASE_COMMAND):
            self.report.add("M4 erase refusal", "SKIP", "no capability bit 13")
            return
        if not self.unit.connected:
            await self.reconnect()
        status = decode_status(await self.unit.read(STATUS))
        newest = status["newest_sequence"]
        if newest == 0 or status["erasing"]:
            self.report.add("M4 erase refusal", "SKIP",
                            "log empty" if newest == 0 else "an erase is already running")
            return
        up_to = newest - 1
        request = encode_erase_log(up_to)
        assert decode_device_control(request)["up_to_sequence"] < newest
        mark = len(self.notes)
        await self.unit.write(DEVICE_CONTROL, request)
        deadline = time.monotonic() + ERASE_WATCH_S
        erasing, lowest = False, newest
        while time.monotonic() < deadline and self.unit.connected:
            await asyncio.sleep(0.5)
            status = decode_status(await self.unit.read(STATUS))
            erasing = erasing or status["erasing"]
            lowest = min(lowest, status["newest_sequence"])
        for _, short, data in self.notes[mark:]:
            if short == STATUS and len(data) == 20:
                note = decode_status(data)
                erasing = erasing or note["erasing"]
                lowest = min(lowest, note["newest_sequence"])
        self.check("M4 erase refusal: up-to below the newest record is refused",
                   not erasing and lowest >= newest,
                   f"newest {newest}, up-to {up_to}; status bit 2 "
                   f"{'SET: the unit accepted it' if erasing else 'never set'}, "
                   f"newest now {status['newest_sequence']} (lowest seen {lowest})")

    async def m4_log(self):
        if not has_cap(self.info, CAP_LOG):
            self.report.add("M4 log download", "SKIP", "no log capability")
            return
        print(f"     full download from 0, batches of {self.args.batch or 'unlimited'}, "
              f"payload {self.unit.payload} (each session waits ~11 s for a measurement)")
        try:
            records, t = await download_all(self.unit, 0, self.args.batch)
        except HarnessError as e:
            self.check("M4 log download: full download", False, str(e))
            return
        transfer = t["seconds"] - sum(t["first_packet_s"])
        rate = len(records) / transfer if transfer > 0 else 0
        self.check("M4 log download: full download ends with END", t["ended"],
                   f"{len(records)} records, {t['bytes']} bytes, {t['seconds']:.1f} s "
                   f"({t['sessions']} sessions, {transfer:.1f} s streaming, {rate:.0f} records/s), "
                   f"next cursor {t['cursor']}")
        self.check("M4 log download: no CRC failures or discarded packets",
                   t["crc_failures"] == 0 and t["discarded"] == 0,
                   f"{t['discarded']} discarded, {t['crc_failures']} CRC failures, "
                   f"{t['counter_skips']} packet-counter skips")
        gaps, dups, disorder = sequence_problems(records)
        self.check("M4 log download: no duplicates, increasing order",
                   dups == 0 and disorder == 0 and t["duplicates"] == 0,
                   f"{dups + t['duplicates']} duplicates, {disorder} out of order")
        lost = sum(hi - lo + 1 for lo, hi in gaps)
        self.check("M4 log download: sequences contiguous", not gaps,
                   (f"{len(gaps)} gaps, {lost} records missing, first {gaps[:5]} "
                    "(allowed after power loss, §7.2)") if gaps else
                   (f"{records[0]['sequence']}..{records[-1]['sequence']}" if records else "log empty"),
                   warn=True)
        status = decode_status(await self.unit.read(STATUS))
        self.check("M4 log download: last record is the newest sequence",
                   not records or records[-1]["sequence"] >= status["newest_sequence"] - 1,
                   f"last {records[-1]['sequence'] if records else '-'}, "
                   f"status newest {status['newest_sequence']}")
        self.reference = records

    async def m2_notify_measurement(self):
        later = lambda c: [t for t, s, _ in self.notes if s == c and t > self.auth_at + 3]  # noqa: E731
        if not (later(READING) and later(STATUS)) and "notify-wait" not in self.args.skip:
            interval = self.config["interval_s"]
            wait = interval + 60 - (time.monotonic() - self.auth_at)
            if wait > 0:
                print(f"     waiting up to {wait:.0f} s for the next measurement "
                      f"(interval {interval} s)")
                deadline = time.monotonic() + wait
                while time.monotonic() < deadline and not (later(READING) and later(STATUS)):
                    await asyncio.sleep(1.0)
                    if not self.unit.connected:
                        break
        if not (later(READING) and later(STATUS)) and "notify-wait" in self.args.skip:
            self.report.add("M2 notify reading and status per measurement", "SKIP", "--quick")
            return
        r, s = later(READING), later(STATUS)
        self.check("M2 notify reading and status per measurement", bool(r and s),
                   f"{len(r)} reading, {len(s)} status notifications after the auth burst")
        bad = [d for _, c, d in self.notes if c in (READING, STATUS) and len(d) != 20]
        self.check("M2 notifications are 20 bytes", not bad, f"{len(bad)} wrong-sized")

    async def m5_unauth_write(self):
        if not self.unit.connected:
            await self.reconnect()
        config = await self.unit.read(CORE_CONFIG)
        name = await self.unit.read(NAME)
        await self.reconnect(authenticate_now=False)
        c = decode_core_config(config)
        c["interval_s"] = 1800 if c["interval_s"] != 1800 else 600  # long either way
        forged = encode_core_config(c)
        await self.unit.write(CORE_CONFIG, forged)
        await self.unit.write(NAME, b"QTest unauth")
        await asyncio.sleep(1.5)
        echoed = await self.unit.read(CORE_CONFIG)
        if echoed == forged:
            self.report.add("M5 unauthenticated write: blank read before auth", "WARN",
                            "the written bytes read back until auth (ArduinoBLE stores them; "
                            "§3 says the value reads as zeros)")
        else:
            self.check("M5 unauthenticated write: blank read before auth", not any(echoed),
                       hexs(echoed))
        await authenticate(self.unit, self.args, self.serial)
        await self.unit.write(EPOCH, encode_epoch(time.time() * 1000))
        await asyncio.sleep(1.0)
        after_config = await self.unit.read(CORE_CONFIG)
        after_name = await self.unit.read(NAME)
        ok = after_config == config and after_name == name
        self.check("M5 unauthenticated writes ignored (read back after auth)", ok,
                   "" if ok else f"config {hexs(after_config)}, name {after_name!r}")
        if not ok:
            await self.restore(config, name)

    async def m4_resume(self):
        records = self.reference
        if records is None:
            self.report.add("M4 log resume after disconnect", "SKIP", "no reference download")
            return
        n = min(self.args.resume_after, len(records) // 2)
        if n < 2:
            self.report.add("M4 log resume after disconnect", "SKIP", f"only {len(records)} records")
            return
        # Start ~3N before the end so the test stays short.
        start = records[max(0, len(records) - 3 * n)]["sequence"]
        ref_max = records[-1]["sequence"]
        reference = {r["sequence"]: r["raw"] for r in records if r["sequence"] >= start}
        if not self.unit.connected:
            await self.reconnect()
        try:
            first = await download_session(self.unit, start, 0, stop_after=n)
        except HarnessError as e:
            self.check("M4 log resume: first part", False, str(e))
            return
        cursor = first.next_cursor()
        await self.unit.disconnect()  # mid-stream, no ABORT
        cut_at = len(first.records)
        await asyncio.sleep(1.0)
        started = time.monotonic()
        await self.reconnect()
        try:
            rest, totals = await download_all(self.unit, cursor, self.args.batch, quiet=True)
        except HarnessError as e:
            self.check("M4 log resume: second part", False, str(e))
            return
        combined = first.records + rest
        gaps, dups, disorder = sequence_problems(combined)
        got = {r["sequence"]: r["raw"] for r in combined if r["sequence"] <= ref_max}
        missing = sorted(set(reference) - set(got))
        extra = sorted(set(got) - set(reference))
        changed = [s for s in set(got) & set(reference) if got[s] != reference[s]]
        self.check("M4 log resume: no gaps or duplicates vs the full download",
                   not missing and not extra and not dups and not disorder and not changed,
                   f"cut after {cut_at} records at cursor {cursor}, resumed in "
                   f"{time.monotonic() - started:.1f} s; {len(got)} of {len(reference)} matched, "
                   f"missing {missing[:5]}, extra {extra[:5]}, {dups} dup, {len(changed)} differ")

    async def m5_reconnect(self):
        times, failures = [], []
        for i in range(self.args.reconnects):
            await self.unit.disconnect()
            await asyncio.sleep(1.0)
            t0 = time.monotonic()
            try:
                self.device = await find_unit(self.args, address=self.device.address, timeout=30)
                found = time.monotonic() - t0
                self.unit = Unit(self.device)
                await self.unit.connect()
                await read_identity(self.unit)
                await authenticate(self.unit, self.args, self.serial)
                await self.unit.write(EPOCH, encode_epoch(time.time() * 1000))
                decode_status(await self.unit.read(STATUS))
                times.append(time.monotonic() - t0)
                print(f"     reconnect {i + 1}: ready in {times[-1]:.2f} s (found in {found:.2f} s)")
            except Exception as e:  # noqa: BLE001
                failures.append(f"#{i + 1}: {e}")
        detail = "; ".join(failures) if failures else (
            f"time to ready min {min(times):.2f} s, median {statistics.median(times):.2f} s, "
            f"max {max(times):.2f} s")
        self.check(f"M5 reconnect and re-authenticate {self.args.reconnects}x", not failures, detail)


async def cmd_run(args):
    suite = Suite(args)
    try:
        await suite.run()
    except HarnessError as e:
        suite.report.add("suite aborted", "FAIL", str(e))
    finally:
        if suite.unit:
            await suite.unit.disconnect()
    return suite.report.summary()


# ====================================================================== main

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true",
                        help="check the codec against PROTOCOL.md's golden vectors and exit")
    parser.add_argument("--name", help="advertised name of the unit")
    parser.add_argument("--address", help="BLE address (CoreBluetooth UUID on macOS)")
    parser.add_argument("--key-file", default=DEFAULT_KEY_FILE)
    parser.add_argument("--scan-time", type=float, default=10.0)
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("scan", help="list Quiesco units with RSSI")
    sub.add_parser("info", help="DIS and device info, unauthenticated")
    sub.add_parser("enrol", help="enrol this Mac (unit on USB power)")
    run = sub.add_parser("run", help="the on-air test suite")
    run.add_argument("--quick", action="store_true",
                     help="skip the measurement wait, log download, resume and reconnects")
    run.add_argument("--skip", action="append", default=[],
                     choices=["robustness", "rapid", "log", "resume", "notify-wait",
                              "unauth-write", "reconnect", "sleep-window", "erase-refusal"])
    run.add_argument("--batch", type=int, default=2000,
                     help="records per download session like the app; 0 = one session")
    run.add_argument("--resume-after", type=int, default=50,
                     help="records to receive before cutting the link in the resume test")
    run.add_argument("--reconnects", type=int, default=5)
    run.add_argument("--preauth-wait", type=float, default=5.0,
                     help="seconds subscribed before auth, expecting no notification")
    download = sub.add_parser("download", help="whole log as CSV (log-report.py format)")
    download.add_argument("out", nargs="?")
    download.add_argument("--out", dest="out_flag")
    download.add_argument("--start", type=int, default=0, help="first sequence wanted")
    download.add_argument("--batch", type=int, default=2000)
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.command is None:
        parser.print_help()
        return 2
    if args.command == "run" and args.quick:
        args.skip += ["notify-wait", "log", "resume", "reconnect"]
    if args.command == "download":
        args.out = args.out_flag or args.out
        if not args.out:
            parser.error("download needs an output file")
    handler = {"scan": cmd_scan, "info": cmd_info, "enrol": cmd_enrol, "run": cmd_run,
               "download": cmd_download}[args.command]

    async def guarded():
        try:
            return await handler(args)
        finally:
            for unit in list(OPEN_UNITS):  # Ctrl-C or error: never leave a link open
                await unit.disconnect()

    try:
        return asyncio.run(guarded())
    except HarnessError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted; disconnected", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
