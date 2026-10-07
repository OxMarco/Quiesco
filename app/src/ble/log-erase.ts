// When to erase the unit's log after a sync ("Erase the unit's log after
// syncing"). Pure, so the rules are tested without a radio.

/** At most one erase a day: each one costs flash wear and ~90 s of work. */
export const LOG_ERASE_MIN_GAP_MS = 24 * 3600_000;

export interface EraseInputs {
  /** The user turned the setting on. */
  enabled: boolean;
  /** The unit has capability logEraseCommand. */
  capable: boolean;
  /** The sync that just ran finished ('done'), rather than failed or stopped. */
  syncDone: boolean;
  /** The phone's saved cursor: the first sequence it still lacks. */
  cursor: number;
  /** The newest sequence the unit holds, from its status. */
  newestSequence: number;
  /** When this phone last had the unit erase its log, or null if never. */
  lastEraseMs: number | null;
  nowMs: number;
}

/**
 * The sequence to erase up to, or null to leave the log alone. Only when the
 * phone holds every record the unit has, so an erase can never lose a night.
 */
export function logEraseUpTo(x: EraseInputs): number | null {
  if (!x.enabled || !x.capable || !x.syncDone) return null;
  const held = x.cursor - 1;
  if (held < 0 || held < x.newestSequence) return null;
  if (x.lastEraseMs !== null && x.nowMs - x.lastEraseMs < LOG_ERASE_MIN_GAP_MS) return null;
  return held;
}
