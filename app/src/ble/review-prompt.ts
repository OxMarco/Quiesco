// When to ask for an App Store / Play rating. Pure, so the rules are tested
// without a store. The OS has the last word: iOS shows the prompt at most
// three times a year and Play keeps its own quota, so a "yes" here can still
// show nothing.

/** Enough nights that the user has seen what the unit is for. */
export const REVIEW_MIN_NIGHTS = 5;

/** Never in the first days: a new user has nothing to judge yet. */
export const REVIEW_MIN_INSTALL_AGE_MS = 3 * 24 * 3600_000;

/** Leave a long gap between asks, whatever the OS would allow. */
export const REVIEW_MIN_GAP_MS = 120 * 24 * 3600_000;

export interface ReviewInputs {
  /** The sync that just ran finished ('done'), rather than failed or stopped. */
  syncDone: boolean;
  /** Distinct nights with data on this phone for the unit. */
  nights: number;
  /** When the app was installed on this phone, or null if unknown. */
  installedMs: number | null;
  /** When this phone last asked, or null if never. */
  lastAskedMs: number | null;
  nowMs: number;
}

/** Ask only after a sync that worked, days after install, once there is a history to like. */
export function shouldAskForReview(x: ReviewInputs): boolean {
  if (!x.syncDone || x.nights < REVIEW_MIN_NIGHTS) return false;
  if (x.installedMs === null || x.nowMs - x.installedMs < REVIEW_MIN_INSTALL_AGE_MS) return false;
  return x.lastAskedMs === null || x.nowMs - x.lastAskedMs >= REVIEW_MIN_GAP_MS;
}
