// Sleep as the phone's health store recorded it (Apple Health, Health Connect),
// turned into what the app judges by: one span per night, and the user's usual
// times. Pure, so the tests cover it; src/health/sleep reads the samples.

import { nightOf, type SleepTimes, STEP_MIN } from './sleep';

export interface SleepSpan {
  startMs: number;
  endMs: number;
}

/** Waking up for a few minutes is still the same night's sleep. */
const SAME_SLEEP_GAP_MS = 45 * 60_000;
/** A nap, or a stray sample, is not a night. */
const MIN_NIGHT_MS = 3 * 3600_000;
/** Fewer nights than this and "usual" is a guess. */
export const MIN_USUAL_NIGHTS = 4;

/** Sorted, with overlapping or nearly touching spans joined. */
export function mergeSpans(spans: SleepSpan[], gapMs = SAME_SLEEP_GAP_MS): SleepSpan[] {
  const sorted = spans.filter((s) => s.endMs > s.startMs).sort((a, b) => a.startMs - b.startMs);
  const out: SleepSpan[] = [];
  for (const s of sorted) {
    const last = out[out.length - 1];
    if (last && s.startMs - last.endMs <= gapMs) last.endMs = Math.max(last.endMs, s.endMs);
    else out.push({ ...s });
  }
  return out;
}

/** Each night's main sleep: the longest merged span starting in it, at least 3 h long. */
export function nightlySleep(spans: SleepSpan[]): Map<string, SleepSpan> {
  const byNight = new Map<string, SleepSpan>();
  for (const s of mergeSpans(spans)) {
    if (s.endMs - s.startMs < MIN_NIGHT_MS) continue;
    const night = nightOf(s.startMs);
    const best = byNight.get(night);
    if (!best || s.endMs - s.startMs > best.endMs - best.startMs) byNight.set(night, s);
  }
  return byNight;
}

/** The night's sleep in Unix seconds, the shape windowForNight returns; null when none was recorded. */
export function sleepForNight(night: string, spans: SleepSpan[]): { fromS: number; toS: number } | null {
  const s = nightlySleep(spans).get(night);
  return s ? { fromS: s.startMs / 1000, toS: s.endMs / 1000 } : null;
}

// Minutes after noon, so a bedtime either side of midnight sorts as one run.
const fromNoon = (ms: number) => {
  const d = new Date(ms);
  return (d.getHours() * 60 + d.getMinutes() + 12 * 60) % (24 * 60);
};
const toClock = (minAfterNoon: number) => (minAfterNoon + 12 * 60) % (24 * 60);
const median = (xs: number[]) => {
  const s = [...xs].sort((a, b) => a - b);
  const mid = s.length >> 1;
  return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
};
const toStep = (min: number) => (Math.round(min / STEP_MIN) * STEP_MIN) % (24 * 60);

/** The usual bed and wake times over these spans, on the 15-minute step; null with too few nights. */
export function usualTimes(spans: SleepSpan[]): SleepTimes | null {
  const nights = [...nightlySleep(spans).values()];
  if (nights.length < MIN_USUAL_NIGHTS) return null;
  return {
    bedMin: toStep(toClock(median(nights.map((n) => fromNoon(n.startMs))))),
    wakeMin: toStep(toClock(median(nights.map((n) => fromNoon(n.endMs))))),
  };
}
