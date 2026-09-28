// What the app says about a metric, and how firm it can be about it. Noise has
// a WHO night guideline, CO₂ is a ventilation indicator, and temperature and
// humidity are comfort, which varies from person to person. Every number here
// comes from references.ts, the same ones the Nights tab judges by; this file
// chooses the words and the evidence behind them.

import type { StoredPoint } from '@/data/db';
import type { Measurement, MetricKey } from '@/protocol/codec';
import { JUDGED, metricValue, roundHalfAway, Severity } from '@/protocol/comfort';

import { pointValue } from './metrics';
import { NOISE_NIGHT_AVG_DB, NOISE_NIGHT_PEAK_DB, REFERENCE_LABEL, REFERENCES, referenceSeverity, type ReferenceKind } from './references';
import type { JudgeMode } from './sleep';
import { deltaToUnit, tempSymbol, type TempUnit, toUnit } from './units';

export type JudgedMetric = Exclude<MetricKey, 'light'>;

/** 3 = health guideline, 2 = ventilation standard, 1 = comfort research. */
export type Firmness = 1 | 2 | 3;

export { NOISE_NIGHT_AVG_DB, NOISE_NIGHT_PEAK_DB };

/**
 * Apparent temperature from the US National Weather Service heat index, in °C.
 * Below 20 °C humidity barely changes how warm a room feels, so it is the air
 * temperature itself.
 */
export function feelsLike(tempC: number, humidityPct: number): number {
  if (tempC < 20) return tempC;
  const f = (tempC * 9) / 5 + 32;
  const rh = humidityPct;
  let hi = 0.5 * (f + 61 + (f - 68) * 1.2 + rh * 0.094);
  if ((hi + f) / 2 >= 80) {
    hi =
      -42.379 +
      2.04901523 * f +
      10.14333127 * rh -
      0.22475541 * f * rh -
      0.00683783 * f * f -
      0.05481717 * rh * rh +
      0.00122874 * f * f * rh +
      0.00085282 * f * rh * rh -
      0.00000199 * f * f * rh * rh;
    if (rh < 13 && f >= 80 && f <= 112) hi -= ((13 - rh) / 4) * Math.sqrt((17 - Math.abs(f - 95)) / 17);
    else if (rh > 85 && f >= 80 && f <= 87) hi += ((rh - 85) / 10) * ((87 - f) / 5);
  }
  return ((hi - 32) * 5) / 9;
}

/** "Feels like", only when it differs enough from the air temperature to say so. */
export function feelsLikeLabel(tempC: number | null, humidityPct: number | null, unit: TempUnit): string | null {
  if (tempC === null || humidityPct === null) return null;
  const feels = feelsLike(tempC, humidityPct);
  return Math.abs(feels - tempC) >= 0.5 ? `Feels ${Math.round(toUnit(feels, unit))}°` : null;
}

/**
 * Daytime noise is judged for hearing, not sleep: the EPA's 70 dB 24-hour
 * average and NIOSH's 85 dBA 8-hour limit.
 */
export const DAY_NOISE = { warnHi: 70, badHi: 85 } as const;

/** The verdict on one metric in the given mode; null where the mode gives none. */
export function judgeFor(metric: MetricKey, value: number | null, mode: JudgeMode): Severity | null {
  if (value === null || metric === 'light') return null;
  if (mode === 'sleep' || metric === 'co2') return referenceSeverity(metric, value);
  if (metric === 'noise') {
    const v = roundHalfAway(value);
    return v > DAY_NOISE.badHi ? Severity.Bad : v > DAY_NOISE.warnHi ? Severity.Warn : Severity.Ok;
  }
  return null;
}

export interface Issue {
  metric: JudgedMetric;
  severity: Severity;
  above: boolean;
}

/**
 * Every judged metric that is off, worst first, ties in the panel's order.
 * Judged by the app's references, which are stricter at night than the
 * panel's all-day bands, so the first issue is not always the one the e-ink
 * face shows.
 */
export function issuesOf(m: Measurement, mode: JudgeMode = 'sleep'): Issue[] {
  const out: Issue[] = [];
  for (const metric of JUDGED as JudgedMetric[]) {
    const value = metricValue(m, metric);
    const severity = judgeFor(metric, value, mode);
    if (value === null || severity === null || severity === Severity.Ok) continue;
    out.push({ metric, severity, above: roundHalfAway(value) > REFERENCES[metric].okHi });
  }
  return out.sort((a, b) => b.severity - a.severity);
}

export function issueHeadline({ metric, severity, above }: Issue, mode: JudgeMode = 'sleep'): string {
  const bad = severity === Severity.Bad;
  switch (metric) {
    case 'co2':
      return bad ? 'Open a window' : 'Getting stuffy';
    case 'temperature':
      return above ? (bad ? 'Too hot for sleep' : 'Warm for sleep') : bad ? 'Too cold for sleep' : 'Cool for sleep';
    case 'humidity':
      return above ? (bad ? 'Too humid' : 'A bit damp') : bad ? 'Too dry' : 'A bit dry';
    case 'noise':
      if (mode === 'day') return bad ? 'Very loud' : 'Loud';
      return bad ? 'Too loud to sleep' : 'A bit loud';
  }
}

export function issueDetail({ metric, severity, above }: Issue, reading: Measurement, unit: TempUnit, mode: JudgeMode = 'sleep'): string {
  const bad = severity === Severity.Bad;
  switch (metric) {
    case 'co2':
      if (mode === 'day') return bad ? 'The air is stale. Open a window now.' : 'Air the room for a few minutes.';
      return bad ? 'The air is stale enough to disturb sleep. Open a window now.' : 'Leave a door or window ajar tonight.';
    case 'temperature': {
      if (!above) return bad ? 'Warm the room a little before bed, or add a blanket.' : 'An extra blanket or warmer sleepwear usually helps.';
      const feels = reading.temperatureC !== null && reading.humidityPct !== null ? feelsLike(reading.temperatureC, reading.humidityPct) : null;
      const lead = feels !== null && feels - reading.temperatureC! >= 0.5 ? `Feels like ${Math.round(toUnit(feels, unit))} ${tempSymbol(unit)} with the humidity. ` : '';
      return `${lead}${bad ? 'Airing the room before bed, a fan or a lighter duvet all help.' : 'A fan or a lighter duvet usually helps.'}`;
    }
    case 'humidity':
      return above
        ? 'Damp air makes warmth feel worse. Air the room or run a dehumidifier.'
        : 'Very dry air can irritate your throat. A humidifier or a bowl of water near a radiator helps.';
    case 'noise':
      if (mode === 'day') {
        return bad
          ? 'Loud enough to harm hearing over time. Turn it down or move away from it.'
          : 'Loud enough to tire your ears over hours. Turn it down or close a door.';
      }
      return bad ? 'Loud enough to wake you. Check windows and doors.' : 'Noticeable noise. Earplugs or a closed door can help.';
  }
}

// --------------------------------------------------------------------- trend

/** How far back the hour-ago point may be from exactly an hour. */
const HOUR_SLACK_S = 15 * 60;

/** Only a change this big is worth a sentence. */
const TREND_MIN: Partial<Record<MetricKey, number>> = { co2: 100, temperature: 0.5, humidity: 3 };

/**
 * The change since an hour before the newest reading, from the log point
 * nearest to that moment; null when the log has none within 15 minutes of it
 * or the metric has no useful trend (noise and light jump about).
 */
export function hourDelta(metric: MetricKey, value: number | null, points: StoredPoint[], nowS: number): number | null {
  if (value === null || !(metric in TREND_MIN)) return null;
  const target = nowS - 3600;
  let best: { v: number; off: number } | null = null;
  for (const p of points) {
    const v = pointValue(p, metric);
    if (v === null) continue;
    const off = Math.abs(p.t - target);
    if (off <= HOUR_SLACK_S && (best === null || off < best.off)) best = { v, off };
  }
  return best === null ? null : value - best.v;
}

/** "Up 310 ppm in the last hour."; null when the change is too small to mention. */
export function trendSentence(metric: MetricKey, delta: number | null, unit: TempUnit): string | null {
  const min = TREND_MIN[metric];
  if (delta === null || min === undefined || Math.abs(delta) < min) return null;
  const dir = delta > 0 ? 'Up' : 'Down';
  const size = Math.abs(delta);
  let amount: string;
  if (metric === 'temperature') {
    const d = deltaToUnit(size, unit);
    amount = `${unit === 'F' ? Math.round(d) : Math.round(d * 2) / 2}°`;
  } else if (metric === 'humidity') amount = `${Math.round(size)} %`;
  else amount = `${Math.round(size / 10) * 10} ppm`;
  return `${dir} ${amount} in the last hour.`;
}

/** Whether the metric's reference is one for the night or a general one, in words. */
export function referenceLabel(metric: MetricKey): string {
  return REFERENCE_LABEL[REFERENCES[metric].kind];
}

/**
 * The metric's reference in words, the same one the Nights tab judges by.
 * By day noise is judged for hearing instead, so it says that.
 */
export function bandSentence(metric: MetricKey, mode: JudgeMode, unit: TempUnit): string {
  const r = REFERENCES[metric];
  switch (metric) {
    case 'co2':
      return `Fresh air is under ${r.okHi} ppm; above ${r.warnHi} ppm the room needs airing.`;
    case 'temperature':
      return `Best for sleep is ${Math.round(toUnit(r.okLo, unit))} – ${Math.round(toUnit(r.okHi, unit))} ${tempSymbol(unit)}.`;
    case 'humidity':
      return `Best for sleep is ${r.okLo} – ${r.okHi} %.`;
    case 'noise':
      return mode === 'sleep'
        ? `At night, single sounds under ${r.okHi} dB and an average under ${NOISE_NIGHT_AVG_DB} dB (WHO).`
        : `By day, under ${DAY_NOISE.warnHi} dB is safe for hearing all day.`;
    case 'light':
      return `At most ${r.okHi} lux while you sleep; above ${r.warnHi} lux is too bright.`;
  }
}

/** The upper edge of the comfortable band in this mode, or null where none applies. */
export function upperLimit(metric: MetricKey, mode: JudgeMode): number | null {
  if (metric === 'co2') return REFERENCES.co2.okHi;
  if (mode === 'sleep') return REFERENCES[metric].okHi;
  return metric === 'noise' ? DAY_NOISE.warnHi : null;
}

/** When the series last rose above the limit and stayed there, or null if it is not above it now. */
export function crossedAbove(metric: MetricKey, points: StoredPoint[], limit: number): number | null {
  let since: number | null = null;
  for (const p of points) {
    const v = pointValue(p, metric);
    if (v === null) continue;
    if (roundHalfAway(v) > limit) since ??= p.t;
    else since = null;
  }
  return since;
}

// ------------------------------------------------------------------ evidence

export interface Zone {
  /** Upper edge of the zone; the last zone runs to the scale's max. */
  to: number;
  tone: 'ok' | 'warn' | 'bad';
  label: string;
}

export interface Evidence {
  title: string;
  /** A reference for the night, or a general one (references.ts). */
  kind: ReferenceKind;
  firmness: Firmness;
  badge: string;
  body: string;
  unit: string;
  scale: { min: number; max: number; zones: Zone[]; ticks: number[] };
  reference: string;
  /** What the Nights tab counts for this metric. */
  counts: string;
  sources: string;
}

/** The metric's scale in the unit it is shown in; only temperature changes. */
export function evidenceScale(metric: JudgedMetric, unit: TempUnit): { scale: Evidence['scale']; unit: string } {
  const e = EVIDENCE[metric];
  if (metric !== 'temperature' || unit === 'C') return { scale: e.scale, unit: e.unit };
  const f = (c: number) => (Number.isFinite(c) ? toUnit(c, unit) : c);
  return {
    scale: {
      min: f(e.scale.min),
      max: f(e.scale.max),
      zones: e.scale.zones.map((z) => ({ ...z, to: f(z.to) })),
      ticks: e.scale.ticks.map(f),
    },
    unit: tempSymbol(unit),
  };
}

/** The zones that fall on the scale, each with its share of the bar (0–1). */
export function scaleParts(scale: Evidence['scale']): (Zone & { share: number })[] {
  const span = scale.max - scale.min;
  const out: (Zone & { share: number })[] = [];
  let from = scale.min;
  for (const z of scale.zones) {
    const to = Math.min(scale.max, z.to);
    if (to > from) out.push({ ...z, share: (to - from) / span });
    from = Math.max(from, to);
  }
  return out;
}

const bandZones = (metric: JudgedMetric, labels: [string, string, string, string, string]): Zone[] => {
  const b = REFERENCES[metric];
  return [
    { to: b.warnLo, tone: 'bad', label: labels[0] },
    { to: b.okLo, tone: 'warn', label: labels[1] },
    { to: b.okHi, tone: 'ok', label: labels[2] },
    { to: b.warnHi, tone: 'warn', label: labels[3] },
    { to: Infinity, tone: 'bad', label: labels[4] },
  ];
};

export const EVIDENCE: Record<JudgedMetric, Evidence> = {
  noise: {
    title: 'Noise at night',
    kind: REFERENCES.noise.kind,
    firmness: 3,
    badge: 'Health guideline · WHO',
    body:
      `For bedrooms at night, the World Health Organization recommends an average under ${NOISE_NIGHT_AVG_DB} dB and single ` +
      `sounds under ${NOISE_NIGHT_PEAK_DB} dB. Louder sounds can wake you or pull you out of deep sleep, even if you don’t remember it.`,
    unit: 'dB',
    scale: {
      min: 20,
      max: 70,
      zones: [
        { to: NOISE_NIGHT_AVG_DB, tone: 'ok', label: 'quiet' },
        { to: NOISE_NIGHT_PEAK_DB, tone: 'warn', label: 'noticeable' },
        { to: REFERENCES.noise.warnHi, tone: 'warn', label: 'can wake you' },
        { to: Infinity, tone: 'bad', label: 'too loud' },
      ],
      ticks: [20, NOISE_NIGHT_AVG_DB, NOISE_NIGHT_PEAK_DB, REFERENCES.noise.warnHi, 70],
    },
    reference: 'For reference: a whisper is about 30 dB, a car passing outside a closed window 45 – 55 dB.',
    counts:
      `Each stretch of two or more readings in a row above ${NOISE_NIGHT_PEAK_DB} dB during your sleep window. The unit listens ` +
      'for about 7 seconds each time it measures, so a short sound between measurements can be missed.',
    sources: 'WHO Guidelines for Community Noise (1999); WHO Night Noise Guidelines for Europe (2009).',
  },
  co2: {
    title: 'CO₂ and fresh air',
    kind: REFERENCES.co2.kind,
    firmness: 2,
    badge: 'Ventilation standard',
    // Wording and numbers from a literature review (Sept 2026): the sleep studies
    // vary ventilation, so CO₂ stands for stale air, not a harm of its own.
    body:
      'The CO₂ in your bedroom comes mostly from your own breathing, so it shows how much fresh air the room is getting. ' +
      `In several small studies, people slept slightly worse (less deep sleep, more time awake) when the room averaged around ` +
      `${REFERENCES.co2.warnHi} ppm or more through the night than in rooms kept under about ${REFERENCES.co2.okHi} ppm. ` +
      'Researchers think stale air in general is the cause, not CO₂ itself, which is not toxic at these levels. The effects are ' +
      'modest and the research is still limited, so take this as a prompt to open a window or door, not as a health alarm.',
    unit: 'ppm',
    scale: {
      min: 400,
      max: 2000,
      zones: [
        { to: REFERENCES.co2.okHi, tone: 'ok', label: 'fresh' },
        { to: REFERENCES.co2.warnHi, tone: 'warn', label: 'stuffy' },
        { to: Infinity, tone: 'bad', label: 'open a window' },
      ],
      ticks: [400, REFERENCES.co2.okHi, REFERENCES.co2.warnHi, 2000],
    },
    reference: 'Outdoor air is about 420 ppm.',
    counts: `When the room first went above ${REFERENCES.co2.okHi} ppm during your sleep window, and the highest reading.`,
    sources:
      'Akimoto et al., Sci Technol Built Environ (2025, ASHRAE 1837-RP); Kang et al., Build Environ (2024); Fan et al., ' +
      'Build Environ (2022); Strøm-Tejsen et al., Indoor Air (2016); Health Canada residential CO₂ guideline (2021); ' +
      'ASHRAE position document on indoor CO₂ (2025).',
  },
  temperature: {
    title: 'Temperature for sleep',
    kind: REFERENCES.temperature.kind,
    firmness: 1,
    badge: 'Comfort research',
    body:
      'Heat makes you wake more and cuts deep and REM sleep, and humidity makes it worse. Most people sleep best in a cool ' +
      'room, but bedding, sleepwear and what you’re used to matter as much as the number, so take the range as a guide rather than a hard limit.',
    unit: '°C',
    scale: {
      min: 14,
      max: 32,
      zones: bandZones('temperature', ['too cold', 'cool', 'comfortable', 'warm', 'too hot']),
      ticks: [14, REFERENCES.temperature.okLo, REFERENCES.temperature.okHi, 32],
    },
    reference: '“Feels like” combines temperature and humidity, the way weather forecasts do.',
    counts: 'Each stretch of two or more readings in a row outside the best-for-sleep range during your sleep window.',
    sources:
      'Okamoto-Mizuno & Mizuno, J Physiol Anthropol (2012); Baniassadi et al., Sci Total Environ (2023); WHO Housing and Health Guidelines (2018).',
  },
  humidity: {
    title: 'Humidity at night',
    kind: REFERENCES.humidity.kind,
    firmness: 1,
    badge: 'Comfort research',
    body:
      'Very dry air can irritate your nose and throat. Damp air makes warmth feel worse and helps mould and dust mites grow.',
    unit: '%',
    scale: {
      min: 0,
      max: 100,
      zones: bandZones('humidity', ['too dry', 'dry', 'comfortable', 'damp', 'too humid']),
      ticks: [0, REFERENCES.humidity.okLo, REFERENCES.humidity.okHi, 100],
    },
    reference: 'Most homes sit between 40 and 60 % for most of the year.',
    counts: `Each stretch of two or more readings in a row outside ${REFERENCES.humidity.okLo} – ${REFERENCES.humidity.okHi} % during your sleep window.`,
    sources: 'US EPA indoor air quality guidance.',
  },
};

export const FIRMNESS_ORDER: { metric: JudgedMetric; label: string; firmness: Firmness; kind: string }[] = [
  { metric: 'noise', label: 'Noise', firmness: 3, kind: 'Health guideline' },
  { metric: 'co2', label: 'CO₂', firmness: 2, kind: 'Ventilation standard' },
  { metric: 'temperature', label: 'Temperature', firmness: 1, kind: 'Comfort, varies by person' },
];
