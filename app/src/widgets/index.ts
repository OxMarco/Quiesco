// Keeps the home-screen widgets current: rebuilds the snapshot from the shown
// unit's newest reading (live while connected, else the newest stored) and
// hands it to the platform's writer (write.ios.ts, write.android.ts).

import { link } from '@/ble/link';
import { loadSession, prefsFor, session, shownUnit } from '@/ble/session';
import * as db from '@/data/db';
import { trace } from '@/debug/trace';
import { fromPoint } from '@/ui/use-reading';
import { judgeMode } from '@/ui/sleep';

import { buildSnapshot, type WidgetSnapshot } from './snapshot';
import { writeSnapshot } from './write';

export type { WidgetSnapshot };

/** The snapshot for the unit on screen, from what the phone has right now. */
export async function currentSnapshot(): Promise<WidgetSnapshot> {
  await loadSession();
  const s = session.get();
  const unit = shownUnit(s);
  const l = link.get();
  const live = unit && l.phase === 'ready' && l.serial === unit.serial && l.reading && l.reading.validMask !== 0 ? l.reading : null;
  const stored = unit && !live ? await db.latestPoint(unit.serial) : null;
  const measuredAt = live ? l.readingAt : stored ? stored.t * 1000 : null;
  return buildSnapshot({
    unit: unit?.name ?? null,
    reading: live ?? (stored ? fromPoint(stored) : null),
    measuredAt,
    mode: judgeMode(Date.now(), prefsFor(s, unit?.serial).sleepWindow),
    tempUnit: s.tempUnit,
  });
}

export async function updateWidgets(): Promise<void> {
  try {
    await writeSnapshot(await currentSnapshot());
  } catch (e) {
    trace('widget update failed', e);
  }
}

const MIN_GAP_MS = 60_000;
let started = false;
let lastRun = 0;
let pending: ReturnType<typeof setTimeout> | null = null;

/** At most one update a minute; the last request in a burst is the one that runs. */
function schedule() {
  if (pending) return;
  const wait = Math.max(0, lastRun + MIN_GAP_MS - Date.now());
  pending = setTimeout(() => {
    pending = null;
    lastRun = Date.now();
    void updateWidgets();
  }, wait);
}

/** From boot: refresh on new data, a unit switch, a new live reading, or °C/°F. */
export function startWidgetUpdates() {
  if (started) return;
  started = true;
  let key = '';
  const check = () => {
    const s = session.get();
    const l = link.get();
    const next = [s.dataVersion, s.selected, s.tempUnit, s.units.length, l.readingAt].join('|');
    if (next === key) return;
    key = next;
    schedule();
  };
  session.subscribe(check);
  link.subscribe(check);
  check();
}
