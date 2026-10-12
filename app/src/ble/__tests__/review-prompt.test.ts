import { REVIEW_MIN_GAP_MS, REVIEW_MIN_INSTALL_AGE_MS, REVIEW_MIN_NIGHTS, shouldAskForReview, type ReviewInputs } from '../review-prompt';

const NOW = Date.UTC(2026, 9, 7, 8);

const base: ReviewInputs = {
  syncDone: true,
  nights: REVIEW_MIN_NIGHTS,
  installedMs: NOW - 30 * 24 * 3600_000,
  lastAskedMs: null,
  nowMs: NOW,
};

describe('ask for a store review', () => {
  test('after a complete sync with enough nights', () => {
    expect(shouldAskForReview(base)).toBe(true);
    expect(shouldAskForReview({ ...base, lastAskedMs: NOW - REVIEW_MIN_GAP_MS })).toBe(true);
  });

  test('never after an incomplete sync', () => {
    expect(shouldAskForReview({ ...base, syncDone: false })).toBe(false);
  });

  test('not before there is a history', () => {
    expect(shouldAskForReview({ ...base, nights: REVIEW_MIN_NIGHTS - 1 })).toBe(false);
  });

  test('not in the first days after install', () => {
    expect(shouldAskForReview({ ...base, installedMs: NOW - REVIEW_MIN_INSTALL_AGE_MS + 1 })).toBe(false);
    expect(shouldAskForReview({ ...base, installedMs: NOW - REVIEW_MIN_INSTALL_AGE_MS })).toBe(true);
    // Unknown install time: stay quiet rather than risk asking a new user.
    expect(shouldAskForReview({ ...base, installedMs: null })).toBe(false);
  });

  test('not again within the gap', () => {
    expect(shouldAskForReview({ ...base, lastAskedMs: NOW - REVIEW_MIN_GAP_MS + 1 })).toBe(false);
  });
});
