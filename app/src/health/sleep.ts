// Sleep from the phone's health store. sleep.ios.ts reads Apple Health and
// sleep.android.ts Health Connect; this fallback (web, tests) has neither.
// Read-only: Quiesco never writes to the health store.

import type { SleepSpan } from '@/ui/health-sleep';

/** The store's name as the user knows it, or null where there is none. */
export const HEALTH_STORE: string | null = null;

export async function healthAvailable(): Promise<boolean> {
  return false;
}

/** Asks once for read access to sleep; true when sleep can be read now. */
export async function requestSleepAccess(): Promise<boolean> {
  return false;
}

/** Recorded sleep overlapping [fromMs, toMs); empty when none or not allowed. */
export async function sleepSpans(_fromMs: number, _toMs: number): Promise<SleepSpan[]> {
  return [];
}
