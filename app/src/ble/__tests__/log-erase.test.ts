import { LOG_ERASE_MIN_GAP_MS, logEraseUpTo, type EraseInputs } from '../log-erase';

const NOW = Date.UTC(2026, 9, 7, 8);

const base: EraseInputs = {
  enabled: true,
  capable: true,
  syncDone: true,
  cursor: 1444,
  newestSequence: 1443,
  lastEraseMs: null,
  nowMs: NOW,
};

describe('erase the log after syncing', () => {
  test('erases up to the last record the phone holds', () => {
    expect(logEraseUpTo(base)).toBe(1443);
    expect(logEraseUpTo({ ...base, lastEraseMs: NOW - LOG_ERASE_MIN_GAP_MS })).toBe(1443);
  });

  test('never when the setting is off or the unit cannot', () => {
    expect(logEraseUpTo({ ...base, enabled: false })).toBeNull();
    expect(logEraseUpTo({ ...base, capable: false })).toBeNull();
  });

  test('never after an incomplete sync', () => {
    expect(logEraseUpTo({ ...base, syncDone: false })).toBeNull();
    // The unit holds a record the phone lacks (one landed after the sync).
    expect(logEraseUpTo({ ...base, newestSequence: 1444 })).toBeNull();
    // Nothing synced yet.
    expect(logEraseUpTo({ ...base, cursor: 0, newestSequence: 0 })).toBeNull();
  });

  test('at most once a day', () => {
    expect(logEraseUpTo({ ...base, lastEraseMs: NOW - 3600_000 })).toBeNull();
    expect(logEraseUpTo({ ...base, lastEraseMs: NOW - LOG_ERASE_MIN_GAP_MS + 1 })).toBeNull();
  });
});
