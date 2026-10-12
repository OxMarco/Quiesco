import { DEFAULT_SLEEP_WINDOW } from '@/ui/sleep';

import {
  BACKGROUND_SYNC_GAP_MS,
  backgroundSyncTargets,
  DEFAULT_UNIT_PREFS,
  newUnitPrefs,
  pickUnit,
  prefsFromLegacy,
  type UnitPrefs,
} from '../units';

const late: UnitPrefs = {
  sleepWindow: { times: { bedMin: 60, wakeMin: 9 * 60 }, set: true },
  panelFollowsSleep: false,
  eraseAfterSync: true,
};

describe('newUnitPrefs', () => {
  it('starts from the defaults on a phone with no unit', () => {
    expect(newUnitPrefs(null)).toEqual(DEFAULT_UNIT_PREFS);
  });

  it('copies the shown unit’s window and panel choice, never the log erase', () => {
    expect(newUnitPrefs(late)).toEqual({ ...late, eraseAfterSync: false });
  });
});

describe('prefsFromLegacy', () => {
  it('is null when nothing was saved', () => {
    expect(prefsFromLegacy({ sleepWindow: null, panelFollowsSleep: undefined, eraseAfterSync: undefined })).toBeNull();
  });

  it('keeps every saved phone-wide setting', () => {
    expect(prefsFromLegacy({ sleepWindow: late.sleepWindow, panelFollowsSleep: false, eraseAfterSync: true })).toEqual(late);
  });

  it('reads times saved under weekday, before weekend times were dropped', () => {
    const p = prefsFromLegacy({ sleepWindow: { weekday: { bedMin: 1380, wakeMin: 420 }, set: true }, panelFollowsSleep: undefined, eraseAfterSync: undefined });
    expect(p?.sleepWindow).toEqual({ times: { bedMin: 1380, wakeMin: 420 }, set: true });
    expect(p?.panelFollowsSleep).toBe(true);
  });

  it('fills a missing window with the default', () => {
    expect(prefsFromLegacy({ sleepWindow: null, panelFollowsSleep: false, eraseAfterSync: undefined })?.sleepWindow).toEqual(DEFAULT_SLEEP_WINDOW);
  });
});

describe('pickUnit', () => {
  const units = [{ serial: 'A' }, { serial: 'B' }];

  it('shows the chosen unit', () => {
    expect(pickUnit(units, 'B')?.serial).toBe('B');
  });

  it('falls back to the first unit when the chosen one was forgotten', () => {
    expect(pickUnit(units, 'C')?.serial).toBe('A');
    expect(pickUnit(units, null)?.serial).toBe('A');
  });

  it('is null with no units', () => {
    expect(pickUnit([], 'A')).toBeNull();
  });
});

describe('backgroundSyncTargets', () => {
  const now = 10 * BACKGROUND_SYNC_GAP_MS;
  const unit = (serial: string, lastSyncMs: number | null, quiet = true) => ({ serial, peripheralId: `p${serial}`, lastSyncMs, quiet });
  const all = new Set(['pA', 'pB', 'pC', 'pD']);

  it('skips the unit on screen', () => {
    expect(backgroundSyncTargets([unit('A', null), unit('B', null)], 'A', all, now).map((u) => u.serial)).toEqual(['B']);
  });

  it('skips units out of range, synced recently, or with nothing to talk to quietly', () => {
    const units = [unit('B', null), unit('C', now - BACKGROUND_SYNC_GAP_MS / 2), unit('D', null, false), unit('E', null)];
    expect(backgroundSyncTargets(units, 'A', all, now).map((u) => u.serial)).toEqual(['B']);
  });

  it('takes the longest unsynced first', () => {
    const units = [unit('B', now - 2 * BACKGROUND_SYNC_GAP_MS), unit('C', null), unit('D', now - 5 * BACKGROUND_SYNC_GAP_MS)];
    expect(backgroundSyncTargets(units, 'A', all, now).map((u) => u.serial)).toEqual(['C', 'D', 'B']);
  });
});
