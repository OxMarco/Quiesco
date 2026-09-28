#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Summarise a log dump (scripts/quiesco-console.py dump log.csv).

  scripts/log-report.py log.csv
  scripts/log-report.py log.csv --reference ref.csv

Reports what a unit can tell about itself without a bench: records lost,
resets, battery drain and a life estimate, sensor ranges and dropouts. With
--reference it compares against readings taken by hand from another
instrument and suggests calibration offsets.

Reference file: CSV with a header naming `time` (ISO 8601 local time, e.g.
2026-10-02T14:30) and any of temperature_c, humidity_pct, co2_ppm, noise_db.
Each row is matched to the unit's record nearest in time (within 10 min).
Standard library only.
"""

import argparse
import csv
import datetime
import math
import statistics
import sys

METRICS = [  # column, validity bit, label, unit
    ("temperature_c", 0, "temperature", "C"),
    ("humidity_pct", 1, "humidity", "%RH"),
    ("pressure_pa", 2, "pressure", "Pa"),
    ("co2_ppm", 3, "CO2", "ppm"),
    ("lux", 4, "light", "lux"),
    ("noise_db", 5, "noise", "dB"),
    ("battery_v", 6, "battery", "V"),
]

# Same resting-voltage curve as the battery screen (src/ui/UiModel.cpp).
BATTERY_CURVE = [(3.30, 0), (3.50, 5), (3.70, 25), (3.80, 50), (3.95, 75), (4.20, 100)]
LOW_BATTERY_V = 3.50  # the firmware's low-battery screen threshold


def battery_percent(volts):
    if volts <= BATTERY_CURVE[0][0]:
        return 0.0
    for (v0, p0), (v1, p1) in zip(BATTERY_CURVE, BATTERY_CURVE[1:]):
        if volts <= v1:
            return p0 + (p1 - p0) * (volts - v0) / (v1 - v0)
    return 100.0


def load(path):
    with open(path) as handle:
        rows = [line for line in handle if not line.startswith("#")]
    records = []
    for row in csv.DictReader(rows):
        record = {
            "sequence": int(row["sequence"]),
            "boot": int(row["boot"]),
            "epoch": int(row["epoch_s"]),
            "uptime_s": int(row["uptime_ms"]) / 1000.0,
            "valid": int(row["valid"]),
        }
        for column, bit, _, _ in METRICS:
            value = row.get(column, "")
            record[column] = float(value) if value and record["valid"] >> bit & 1 else None
        for column in ("scd_temperature_c", "scd_humidity_pct"):
            value = row.get(column, "")
            record[column] = float(value) if value else None
        records.append(record)
    return records


def assign_times(records):
    """Wall time for every record that can be dated (PROTOCOL.md §7.4)."""
    offsets = {}
    for r in records:
        if r["epoch"] and r["boot"] and r["boot"] not in offsets:
            offsets[r["boot"]] = r["epoch"] - r["uptime_s"]
    undated = 0
    for r in records:
        if r["epoch"]:
            r["time"] = float(r["epoch"])
        elif r["boot"] in offsets:
            r["time"] = offsets[r["boot"]] + r["uptime_s"]
        else:
            r["time"] = None
            undated += 1
    return undated


def fmt_time(seconds):
    return datetime.datetime.fromtimestamp(seconds).strftime("%Y-%m-%d %H:%M")


def slope_per_day(points):
    """Least-squares slope of (time s, value) points, per day."""
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx * 86400


def report_integrity(records):
    print("== Integrity")
    first, last = records[0]["sequence"], records[-1]["sequence"]
    missing = []
    for a, b in zip(records, records[1:]):
        if b["sequence"] != a["sequence"] + 1:
            missing.append((a["sequence"] + 1, b["sequence"] - 1))
    lost = sum(hi - lo + 1 for lo, hi in missing)
    print(f"records {len(records)}, sequences {first}..{last}, missing {lost}")
    for lo, hi in missing[:10]:
        print(f"  gap {lo}..{hi}" if hi > lo else f"  gap {lo}")
    if len(missing) > 10:
        print(f"  ... {len(missing) - 10} more gaps")
    boots = []
    for r in records:
        if not boots or boots[-1][0] != r["boot"]:
            boots.append([r["boot"], 0])
        boots[-1][1] += 1
    print(f"boots {len(boots)}: " + ", ".join(
        f"#{b or '?'} ({n} records)" for b, n in boots[:12]) +
        (" ..." if len(boots) > 12 else ""))
    if len(boots) > 1:
        print("  more than one boot: check diagnostics (000F) or the debug boot "
              "line for the reset cause; a watchdog reset is a bug")


def report_time(records, undated):
    print("\n== Time")
    dated = [r for r in records if r["time"] is not None]
    if not dated:
        print("no record can be dated: the clock was never synced (write epoch "
              "0005 from the phone on every connect)")
        return
    span = dated[-1]["time"] - dated[0]["time"]
    print(f"{fmt_time(dated[0]['time'])} .. {fmt_time(dated[-1]['time'])} "
          f"({span / 86400:.1f} days); {undated} records cannot be dated")
    intervals = [b["time"] - a["time"] for a, b in zip(dated, dated[1:])
                 if b["boot"] == a["boot"] and b["sequence"] == a["sequence"] + 1]
    if intervals:
        median = statistics.median(intervals)
        late = sum(1 for i in intervals if i > median * 1.5)
        print(f"measurement interval median {median:.0f} s, {late} intervals "
              f"over 1.5x (missed or delayed cycles)")


def report_battery(records):
    print("\n== Battery")
    points = [(r["time"], r["battery_v"]) for r in records
              if r["time"] is not None and r["battery_v"] is not None]
    if len(points) < 2:
        print("not enough dated battery readings")
        return
    # The longest stretch without a rise of more than 50 mV is taken as one
    # discharge on battery; a larger rise means the charger was plugged in.
    segments, current = [], [points[0]]
    for prev, point in zip(points, points[1:]):
        if point[1] - prev[1] > 0.05:
            segments.append(current)
            current = []
        current.append(point)
    segments.append(current)
    discharge = max(segments, key=lambda s: s[-1][0] - s[0][0])
    days = (discharge[-1][0] - discharge[0][0]) / 86400
    v0, v1 = discharge[0][1], discharge[-1][1]
    print(f"longest discharge: {days:.2f} days, {v0:.3f} V -> {v1:.3f} V "
          f"({battery_percent(v0):.0f}% -> {battery_percent(v1):.0f}%)")
    if days < 1:
        print("run on battery for at least a few days for a useful estimate")
        return
    used = battery_percent(v0) - battery_percent(v1)
    if used <= 0.5:
        print("no measurable drop yet; keep running")
        return
    per_day = used / days
    usable = battery_percent(4.20) - battery_percent(LOW_BATTERY_V)
    print(f"about {per_day:.1f}% per day -> roughly {usable / per_day:.0f} days "
          f"from full to the low-battery screen ({LOW_BATTERY_V} V)")
    print("  rough: LiPo voltage is a poor fuel gauge, and self-discharge, "
          "temperature and BLE use all move it; compare 60 s and 300 s runs")


def report_sensors(records):
    print("\n== Sensors")
    # A record with no valid reading at all is a stress-test filler ('w'),
    # not a sensor dropout.
    measured = [r for r in records if r["valid"]]
    if len(measured) < len(records):
        print(f"{len(records) - len(measured)} empty records (stress-test "
              f"fillers) left out below")
    records = measured
    for column, bit, label, unit in METRICS:
        values = [r[column] for r in records if r[column] is not None]
        dropped = len(records) - len(values)
        if not values:
            print(f"{label:12} no valid readings")
            continue
        print(f"{label:12} min {min(values):10.2f}  mean {statistics.fmean(values):10.2f}"
              f"  max {max(values):10.2f} {unit:4} missing {dropped}")


def dew_point(temperature_c, humidity_pct):
    """Magnus dew point: the same air gives the same dew point at any
    temperature, so it compares two humidity sensors that sit at different
    temperatures."""
    gamma = math.log(max(humidity_pct, 0.1) / 100.0) + \
        17.62 * temperature_c / (243.12 + temperature_c)
    return 243.12 * gamma / (17.62 - gamma)


def report_sensor_pair(records):
    pairs = [r for r in records
             if r["scd_temperature_c"] is not None and r["temperature_c"] is not None
             and r["humidity_pct"] is not None]
    if not pairs:
        return
    print("\n== BME280 vs SCD41 (cross-check)")
    print("BME280 values include the calibration offsets; SCD41 values are raw "
          "and self-heated by its CO2 emitter")
    t_diff = [r["scd_temperature_c"] - r["temperature_c"] for r in pairs]
    dp_diff = [dew_point(r["scd_temperature_c"], r["scd_humidity_pct"]) -
               dew_point(r["temperature_c"], r["humidity_pct"]) for r in pairs]
    for label, diffs in (("temperature", t_diff), ("dew point", dp_diff)):
        mean = statistics.fmean(diffs)
        spread = statistics.pstdev(diffs) if len(diffs) > 1 else 0.0
        outliers = sum(1 for d in diffs if abs(d - mean) > 3 * spread + 0.5)
        print(f"{label:12} SCD41 - BME280: mean {mean:+.2f} C, spread {spread:.2f}, "
              f"{outliers} records off by more than 3 spreads + 0.5 C "
              f"({len(diffs)} pairs)")
    print("  a steady offset is self-heating; a jump or drift in the difference "
          "means one sensor changed. Matching dew points mean both humidity "
          "sensors see the same air")


def parse_reference_time(text):
    moment = datetime.datetime.fromisoformat(text)
    return moment.timestamp()


def report_reference(records, path):
    print("\n== Reference comparison")
    dated = [r for r in records if r["time"] is not None]
    if not dated:
        print("the log has no dated records to match against")
        return
    with open(path) as handle:
        rows = list(csv.DictReader(handle))
    for column, _, label, unit in METRICS:
        if not rows or column not in rows[0]:
            continue
        differences = []
        for row in rows:
            if not row.get(column):
                continue
            when = parse_reference_time(row["time"])
            nearest = min(dated, key=lambda r: abs(r["time"] - when))
            if abs(nearest["time"] - when) > 600 or nearest[column] is None:
                continue
            differences.append(nearest[column] - float(row[column]))
        if not differences:
            print(f"{label:12} no reference row within 10 min of a record")
            continue
        mean = statistics.fmean(differences)
        spread = statistics.pstdev(differences) if len(differences) > 1 else 0.0
        print(f"{label:12} unit - reference: mean {mean:+.2f} {unit}, "
              f"spread {spread:.2f}, {len(differences)} pairs")
        if column in ("temperature_c", "humidity_pct", "noise_db"):
            print(f"{'':12} suggested offset {-mean:+.2f} {unit} (add to the "
                  f"current offset in 0007; offsets already applied are in the log)")
        elif column == "co2_ppm" and abs(mean) > 50:
            print(f"{'':12} consider an FRC against the reference (000B)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--reference")
    args = parser.parse_args()
    records = load(args.log)
    if not records:
        sys.exit("no records in the log")
    records.sort(key=lambda r: r["sequence"])
    undated = assign_times(records)
    report_integrity(records)
    report_time(records, undated)
    report_battery(records)
    report_sensors(records)
    report_sensor_pair(records)
    if args.reference:
        report_reference(records, args.reference)


if __name__ == "__main__":
    main()
