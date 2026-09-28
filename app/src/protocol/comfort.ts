// Comfort policy, ported from firmware/src/ui/ComfortEvaluation.cpp and
// UiModel.cpp so the phone always agrees with the e-ink panel (UI.md §3).

import type { Measurement, MetricKey } from './codec';

export enum Severity {
  Ok = 0,
  Warn = 1,
  Bad = 2,
}

interface Band {
  warnLo: number;
  okLo: number;
  okHi: number;
  warnHi: number;
}

const NO_LOW = -1e9;

export const BANDS: Record<MetricKey, Band> = {
  co2: { warnLo: NO_LOW, okLo: NO_LOW, okHi: 800, warnHi: 1200 },
  temperature: { warnLo: 18, okLo: 20, okHi: 26, warnHi: 28 },
  humidity: { warnLo: 25, okLo: 30, okHi: 60, warnHi: 70 },
  noise: { warnLo: NO_LOW, okLo: NO_LOW, okHi: 55, warnHi: 70 },
  light: { warnLo: NO_LOW, okLo: NO_LOW, okHi: 1e9, warnHi: 1e9 },
};

/** Judged metrics, in tie-break order. Light is informational only. */
export const JUDGED: readonly MetricKey[] = ['co2', 'temperature', 'humidity', 'noise'];

/** The firmware's roundToInt: half away from zero. */
export function roundHalfAway(value: number): number {
  return Math.trunc(value + (value >= 0 ? 0.5 : -0.5));
}

export function bandSeverity(metric: MetricKey, value: number): Severity {
  const b = BANDS[metric];
  if (value < b.warnLo || value > b.warnHi) return Severity.Bad;
  if (value < b.okLo || value > b.okHi) return Severity.Warn;
  return Severity.Ok;
}

export function metricValue(m: Measurement, metric: MetricKey): number | null {
  switch (metric) {
    case 'co2':
      return m.co2Ppm;
    case 'temperature':
      return m.temperatureC;
    case 'humidity':
      return m.humidityPct;
    case 'noise':
      return m.noiseDb;
    case 'light':
      return m.lux;
  }
}

/** Values are rounded before judging, so the band matches the printed number. */
export function judge(metric: MetricKey, value: number | null): Severity | null {
  return value === null ? null : bandSeverity(metric, roundHalfAway(value));
}

export interface Verdict {
  severity: Severity;
  /** The worst judged metric; null when everything is comfortable or nothing is valid. */
  metric: MetricKey | null;
  above: boolean;
  /** False when no judged metric is valid (the device's "unavailable" screen). */
  available: boolean;
}

export function verdict(m: Measurement): Verdict {
  let worst: Verdict = { severity: Severity.Ok, metric: null, above: false, available: false };
  for (const metric of JUDGED) {
    const value = metricValue(m, metric);
    if (value === null) continue;
    worst.available = true;
    const rounded = roundHalfAway(value);
    const severity = bandSeverity(metric, rounded);
    if (severity > worst.severity) {
      worst = { severity, metric, above: rounded > BANDS[metric].okHi, available: true };
    }
  }
  return worst;
}

/** The nudge the panel prints for the worst metric (UI.md §3). */
export function nudge(v: Verdict): string | null {
  if (!v.metric || v.severity === Severity.Ok) return null;
  const bad = v.severity === Severity.Bad;
  switch (v.metric) {
    case 'co2':
      return bad ? 'Open a window' : 'Getting stuffy';
    case 'temperature':
      return v.above ? (bad ? 'Too hot' : 'A bit warm') : bad ? 'Too cold' : 'A bit chilly';
    case 'humidity':
      return v.above ? (bad ? 'Too humid' : 'A bit damp') : bad ? 'Too dry' : 'A bit dry';
    case 'noise':
      return bad ? 'Too loud' : 'A bit loud';
    case 'light':
      return null;
  }
}

/** 1S LiPo resting-voltage curve, as the firmware (UiModel.cpp). */
const BATTERY_CURVE: [number, number][] = [
  [3.3, 0],
  [3.5, 5],
  [3.7, 25],
  [3.8, 50],
  [3.95, 75],
  [4.2, 100],
];

export function batteryPercent(volts: number): number {
  if (volts <= BATTERY_CURVE[0][0]) return 0;
  for (let i = 1; i < BATTERY_CURVE.length; i++) {
    const [hv, hp] = BATTERY_CURVE[i];
    if (volts <= hv) {
      const [lv, lp] = BATTERY_CURVE[i - 1];
      return Math.trunc(lp + ((volts - lv) / (hv - lv)) * (hp - lp) + 0.5);
    }
  }
  return 100;
}
