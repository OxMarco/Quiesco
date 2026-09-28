// A night's story, judged only inside the sleep window (sleep.ts): for each
// metric, the spells it spent outside its reference (references.ts).

import type { StoredPoint } from '@/data/db';
import type { MetricKey } from '@/protocol/codec';
import { roundHalfAway, Severity } from '@/protocol/comfort';

import { feelsLike } from './advice';
import { pointValue } from './metrics';
import { type Reference, REFERENCES, referenceSeverity } from './references';
import { formatTime, nightKey } from './sleep';

type BandedMetric = 'noise' | 'co2' | 'temperature' | 'humidity' | 'light';

/** The numbers a night is judged by: the app's references (references.ts). */
export const NIGHT_BANDS: Record<BandedMetric, Reference> = REFERENCES;

export const nightSeverity = referenceSeverity;

export function nightLabel(night: string): string {
  const [y, m, d] = night.split('-').map(Number);
  const evening = new Date(y, m - 1, d);
  const today = new Date();
  if (night === nightKey(new Date(today.getFullYear(), today.getMonth(), today.getDate() - 1))) return 'Last night';
  if (night === nightKey(today)) return 'Tonight';
  return evening.toLocaleDateString([], { weekday: 'short', day: 'numeric', month: 'short' });
}

export interface Moment {
  t: number;
  v: number;
}

/**
 * A stretch of readings outside the sleep band. A single reading is not a
 * spell (it may be a door slam or sensor noise): it takes MIN_READINGS in a
 * row to start one, and as many back in the band to end it.
 */
export interface Spell {
  fromS: number;
  /** The first reading back in the band; null when it lasted to (nearly) the end of the window. */
  toS: number | null;
  above: boolean;
  /** The worst level held for at least MIN_READINGS in a row. */
  severity: Severity;
}

const MIN_READINGS = 2;

/** A spell ending this close to wake-up reads as lasting until it. */
const END_SLACK_S = 15 * 60;

export interface NightStory {
  /** Readings inside the sleep window. */
  samples: number;
  noise: {
    /** Energy average (Leq) of the window's readings, dB. */
    average: number | null;
    peak: Moment | null;
    spells: Spell[];
  };
  co2: { peak: Moment | null; spells: Spell[] };
  temperature: { min: number; max: number; feelsMax: number | null; spells: Spell[] } | null;
  humidity: { min: number; max: number; spells: Spell[] } | null;
  light: { peak: Moment; spells: Spell[] } | null;
}

function range(values: number[]): { min: number; max: number } | null {
  return values.length ? { min: Math.min(...values), max: Math.max(...values) } : null;
}

function peakOf(night: StoredPoint[], metric: MetricKey): Moment | null {
  let peak: Moment | null = null;
  for (const p of night) {
    const v = pointValue(p, metric);
    if (v !== null && (!peak || v > peak.v)) peak = { t: p.t, v };
  }
  return peak;
}

/** Runs of readings outside the band on one side, debounced by MIN_READINGS both ways. */
function bandSpells(night: StoredPoint[], metric: BandedMetric, endS: number): Spell[] {
  const rows = night.flatMap((p) => {
    const v = pointValue(p, metric as MetricKey);
    if (v === null) return [];
    const severity = nightSeverity(metric, v);
    return [{ t: p.t, severity, above: severity === Severity.Ok ? null : roundHalfAway(v) > NIGHT_BANDS[metric].okHi }];
  });

  // Raw runs (inclusive row indices), then drop the short ones and bridge short gaps.
  const runs: { from: number; to: number; above: boolean }[] = [];
  rows.forEach((r, i) => {
    if (r.above === null) return;
    const last = runs[runs.length - 1];
    if (last && last.above === r.above && last.to === i - 1) last.to = i;
    else runs.push({ from: i, to: i, above: r.above });
  });
  const merged: typeof runs = [];
  for (const run of runs.filter((r) => r.to - r.from + 1 >= MIN_READINGS)) {
    const last = merged[merged.length - 1];
    if (last && last.above === run.above && run.from - last.to - 1 < MIN_READINGS) last.to = run.to;
    else merged.push({ ...run });
  }

  return merged.map((run) => {
    let severity = Severity.Ok;
    for (let i = run.from; i < run.to; i++) {
      severity = Math.max(severity, Math.min(rows[i].severity, rows[i + 1].severity));
    }
    const after = rows[run.to + 1];
    return {
      fromS: rows[run.from].t,
      toS: after && after.t < endS - END_SLACK_S ? after.t : null,
      above: run.above,
      severity,
    };
  });
}

export function analyzeNight(points: StoredPoint[], window: { fromS: number; toS: number }): NightStory {
  const night = points.filter((p) => p.t >= window.fromS && p.t <= window.toS);
  const spells = (metric: BandedMetric) => bandSpells(night, metric, window.toS);

  const noises = night.flatMap((p) => (p.noise === null ? [] : [p.noise]));
  const average = noises.length
    ? 10 * Math.log10(noises.reduce((sum, db) => sum + 10 ** (db / 10), 0) / noises.length)
    : null;

  const temps = range(night.flatMap((p) => (p.temperature === null ? [] : [p.temperature])));
  const feels = night.flatMap((p) => (p.temperature === null || p.humidity === null ? [] : [feelsLike(p.temperature, p.humidity)]));
  const humidity = range(night.flatMap((p) => (p.humidity === null ? [] : [p.humidity])));
  const lightPeak = peakOf(night, 'light');

  return {
    samples: night.length,
    noise: { average, peak: peakOf(night, 'noise'), spells: spells('noise') },
    co2: { peak: peakOf(night, 'co2'), spells: spells('co2') },
    temperature: temps && { ...temps, feelsMax: feels.length ? Math.max(...feels) : null, spells: spells('temperature') },
    humidity: humidity && { ...humidity, spells: spells('humidity') },
    light: lightPeak && { peak: lightPeak, spells: spells('light') },
  };
}

const SPELL_WORDS: Record<BandedMetric, (s: Spell) => string> = {
  noise: (s) => (s.severity === Severity.Bad ? 'Noisy' : 'A bit noisy'),
  co2: () => 'Stuffy',
  temperature: (s) =>
    s.above ? (s.severity === Severity.Bad ? 'Too hot' : 'A bit warm') : s.severity === Severity.Bad ? 'Too cold' : 'A bit chilly',
  humidity: (s) =>
    s.above ? (s.severity === Severity.Bad ? 'Too humid' : 'A bit damp') : s.severity === Severity.Bad ? 'Too dry' : 'A bit dry',
  light: (s) => (s.severity === Severity.Bad ? 'Too bright' : 'Dim light'),
};

const ALL_CLEAR: Record<BandedMetric, string> = {
  noise: 'Calm all night',
  co2: 'Fresh all night',
  temperature: 'Optimal',
  humidity: 'Optimal',
  light: 'Dark all night',
};

/**
 * The card's one-line report: the worst spell ("Too hot from 08:00 to 10:20",
 * or "Stuffy from 08:20" when it lasted to wake), how many others there were,
 * and the severity to colour it with.
 */
export function spellReport(
  metric: BandedMetric,
  spells: Spell[],
  window: { toS: number },
): { text: string; others: number; severity: Severity } {
  if (spells.length === 0) return { text: ALL_CLEAR[metric], others: 0, severity: Severity.Ok };
  const length = (s: Spell) => (s.toS ?? window.toS) - s.fromS;
  const worst = spells.reduce((a, b) => (b.severity > a.severity || (b.severity === a.severity && length(b) > length(a)) ? b : a));
  const until = worst.toS === null ? '' : ` to ${formatTime(worst.toS)}`;
  return {
    text: `${SPELL_WORDS[metric](worst)} from ${formatTime(worst.fromS)}${until}`,
    others: spells.length - 1,
    severity: worst.severity,
  };
}
