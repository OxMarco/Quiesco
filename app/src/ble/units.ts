// More than one unit per phone: which one the app shows, what each keeps on
// the phone, and which others to sync in the background. Pure, so the rules
// are tested without a radio or a database.

import { DEFAULT_SLEEP_WINDOW, type SleepWindow } from '@/ui/sleep';

/** What the phone keeps per unit: each one may sit in a room with its own sleeper. */
export interface UnitPrefs {
  sleepWindow: SleepWindow;
  /** The unit's panel judges by time of day like the app (capability sleepWindow). */
  panelFollowsSleep: boolean;
  /** Erase the unit's log once it is all on this phone (capability logEraseCommand). */
  eraseAfterSync: boolean;
}

export const DEFAULT_UNIT_PREFS: UnitPrefs = {
  sleepWindow: DEFAULT_SLEEP_WINDOW,
  panelFollowsSleep: true,
  eraseAfterSync: false,
};

/**
 * A new unit starts from the one on screen: most homes share a rhythm. Erasing
 * the unit's log is never copied: it puts the history only on this phone, so
 * it has to be a choice made for that unit.
 */
export function newUnitPrefs(template: UnitPrefs | null): UnitPrefs {
  if (!template) return DEFAULT_UNIT_PREFS;
  return { sleepWindow: template.sleepWindow, panelFollowsSleep: template.panelFollowsSleep, eraseAfterSync: false };
}

/** Phone-wide settings saved before units had their own, as read from storage. */
export interface LegacyPrefs {
  sleepWindow: (Partial<SleepWindow> & { weekday?: SleepWindow['times'] }) | null;
  panelFollowsSleep: unknown;
  eraseAfterSync: unknown;
}

/** The old phone-wide settings as one unit's, or null when none were saved. */
export function prefsFromLegacy(legacy: LegacyPrefs): UnitPrefs | null {
  // Saved before weekend times were dropped, the times were under `weekday`.
  const times = legacy.sleepWindow?.times ?? legacy.sleepWindow?.weekday;
  const follows = typeof legacy.panelFollowsSleep === 'boolean' ? legacy.panelFollowsSleep : null;
  const erase = typeof legacy.eraseAfterSync === 'boolean' ? legacy.eraseAfterSync : null;
  if (!times && follows === null && erase === null) return null;
  return {
    sleepWindow: times ? { times, set: legacy.sleepWindow?.set ?? true } : DEFAULT_UNIT_PREFS.sleepWindow,
    panelFollowsSleep: follows ?? DEFAULT_UNIT_PREFS.panelFollowsSleep,
    eraseAfterSync: erase ?? DEFAULT_UNIT_PREFS.eraseAfterSync,
  };
}

/** The unit the app shows: the chosen one, or the first added if that is gone. */
export function pickUnit<U extends { serial: string }>(units: U[], selected: string | null): U | null {
  return units.find((u) => u.serial === selected) ?? units[0] ?? null;
}

/** Leave a unit alone in the background if it synced this recently. */
export const BACKGROUND_SYNC_GAP_MS = 30 * 60_000;

export interface BackgroundCandidate {
  serial: string;
  peripheralId: string;
  lastSyncMs: number | null;
  /** False for a unit this phone holds no key for: nothing to talk to quietly. */
  quiet: boolean;
}

/**
 * The other units worth a quiet sync, oldest sync first: in range, not on
 * screen (that one syncs when it connects), and not synced in the last half hour.
 */
export function backgroundSyncTargets<U extends BackgroundCandidate>(
  units: U[],
  shown: string | null,
  inRange: ReadonlySet<string>,
  nowMs: number,
): U[] {
  return units
    .filter(
      (u) =>
        u.quiet &&
        u.serial !== shown &&
        inRange.has(u.peripheralId) &&
        (u.lastSyncMs === null || nowMs - u.lastSyncMs >= BACKGROUND_SYNC_GAP_MS),
    )
    .sort((a, b) => (a.lastSyncMs ?? 0) - (b.lastSyncMs ?? 0));
}
