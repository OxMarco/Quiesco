// The sleep window: the hours a night is judged over. Everything outside it
// (the TV at 22:00, a sunny late morning) is still drawn but never counted.
// Times are minutes after local midnight; a "night" is named by the local date
// of the evening that starts it (YYYY-MM-DD), as db.nightsWithData returns.

import type { StoredPoint } from '@/data/db';

export interface SleepTimes {
  bedMin: number;
  wakeMin: number;
}

export interface SleepWindow {
  weekday: SleepTimes;
  /** Friday and Saturday nights, when they differ. */
  weekend: SleepTimes | null;
  /** False until the user has saved a window; the default is only a guess. */
  set: boolean;
}

export const DEFAULT_SLEEP_WINDOW: SleepWindow = {
  weekday: { bedMin: 23 * 60 + 30, wakeMin: 7 * 60 },
  weekend: null,
  set: false,
};

export const STEP_MIN = 15;
export const MIN_SLEEP_MIN = 3 * 60;
export const MAX_SLEEP_MIN = 14 * 60;

const DAY_MIN = 24 * 60;

export function wrapMinutes(min: number): number {
  return ((min % DAY_MIN) + DAY_MIN) % DAY_MIN;
}

export function durationMin(t: SleepTimes): number {
  return wrapMinutes(t.wakeMin - t.bedMin);
}

export function validTimes(t: SleepTimes): boolean {
  const d = durationMin(t);
  return d >= MIN_SLEEP_MIN && d <= MAX_SLEEP_MIN;
}

function nightDate(night: string): [number, number, number] {
  const [y, m, d] = night.split('-').map(Number);
  return [y, m - 1, d];
}

export function nightKey(date: Date): string {
  const y = date.getFullYear();
  const m = String(date.getMonth() + 1).padStart(2, '0');
  const d = String(date.getDate()).padStart(2, '0');
  return `${y}-${m}-${d}`;
}

/** The night a moment belongs to, the same rule as db.nightsWithData: its time − 12 h. */
export function nightOf(ms: number): string {
  return nightKey(new Date(ms - 12 * 3600_000));
}

export function isWeekendNight(night: string): boolean {
  const [y, m, d] = nightDate(night);
  const day = new Date(y, m, d).getDay();
  return day === 5 || day === 6;
}

export function timesForNight(night: string, w: SleepWindow): SleepTimes {
  return w.weekend && isWeekendNight(night) ? w.weekend : w.weekday;
}

/** Unix seconds. A bedtime before noon falls in the small hours after the evening. */
export function windowForNight(night: string, w: SleepWindow): { fromS: number; toS: number } {
  const t = timesForNight(night, w);
  const [y, m, d] = nightDate(night);
  const day = t.bedMin < 12 * 60 ? d + 1 : d;
  // Date folds overflowing minutes into hours and days, in local time.
  const from = new Date(y, m, day, 0, t.bedMin);
  const to = new Date(y, m, day, 0, t.bedMin + durationMin(t));
  return { fromS: from.getTime() / 1000, toS: to.getTime() / 1000 };
}

/** What a night's charts show: the window plus some evening before and morning after. */
export function viewForNight(night: string, w: SleepWindow): { fromS: number; toS: number } {
  const { fromS, toS } = windowForNight(night, w);
  return { fromS: fromS - 90 * 60, toS: toS + 60 * 60 };
}

export function formatClock(min: number): string {
  const d = new Date(2000, 0, 1, 0, wrapMinutes(min));
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

export function formatTime(s: number): string {
  return new Date(s * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

export function formatDuration(min: number): string {
  const h = Math.floor(min / 60);
  const m = min % 60;
  if (h === 0) return `${m} min`;
  return m ? `${h} h ${m} min` : `${h} h`;
}

export type SleepPhase =
  | { kind: 'before'; inMin: number; fromS: number; toS: number }
  | { kind: 'during'; fromS: number; toS: number };

/** Where "now" sits relative to tonight's window, for the Room screen. */
export function sleepPhase(nowMs: number, w: SleepWindow): SleepPhase {
  let night = nightOf(nowMs);
  let win = windowForNight(night, w);
  const nowS = nowMs / 1000;
  if (nowS >= win.toS) {
    const [y, m, d] = nightDate(night);
    night = nightKey(new Date(y, m, d + 1));
    win = windowForNight(night, w);
  }
  if (nowS >= win.fromS) return { kind: 'during', ...win };
  return { kind: 'before', inMin: Math.round((win.fromS - nowS) / 60), ...win };
}

/** Comfort verdicts for sleep start this long before bedtime. */
export const VERDICT_LEAD_MIN = 60;

/**
 * Which verdicts apply now. By day only CO₂ and noise are judged, since stale
 * air and harmful noise matter at any hour; from an hour before bedtime until
 * wake every metric is judged on the sleep bands.
 */
export type JudgeMode = 'day' | 'sleep';

export function judgeMode(nowMs: number, w: SleepWindow): JudgeMode {
  const phase = sleepPhase(nowMs, w);
  return phase.kind === 'during' || phase.inMin <= VERDICT_LEAD_MIN ? 'sleep' : 'day';
}

// ------------------------------------------------------ lights-out suggestion

/** Below this the room counts as dark (a phone screen or night light reads less). */
const DARK_LUX = 3;
const DARK_HOLD_S = 30 * 60;
const MIN_NIGHTS = 4;

/**
 * When the lights usually go out on weekday nights, rounded to the step, or
 * null when there are too few nights to say. Looks up to 3 h either side of
 * the current bedtime for a lit room turning dark and staying dark for 30 min.
 */
export function typicalLightsOut(points: StoredPoint[], nights: string[], w: SleepWindow): number | null {
  const found: number[] = [];
  for (const night of nights) {
    if (w.weekend && isWeekendNight(night)) continue;
    const { fromS } = windowForNight(night, w);
    const lit = points.filter((p) => p.lux !== null && p.t >= fromS - 3 * 3600 && p.t <= fromS + 3 * 3600);
    for (let i = 1; i < lit.length; i++) {
      if (lit[i - 1].lux! < DARK_LUX || lit[i].lux! >= DARK_LUX) continue;
      const hold = lit.filter((p) => p.t > lit[i].t && p.t <= lit[i].t + DARK_HOLD_S);
      if (hold.length === 0 || hold.some((p) => p.lux! >= DARK_LUX)) continue;
      // Minutes since the bedtime, so nights crossing midnight compare directly.
      found.push((lit[i].t - fromS) / 60);
      break;
    }
  }
  if (found.length < MIN_NIGHTS) return null;
  found.sort((a, b) => a - b);
  const median = found[Math.floor(found.length / 2)];
  return wrapMinutes(Math.round((w.weekday.bedMin + median) / STEP_MIN) * STEP_MIN);
}
