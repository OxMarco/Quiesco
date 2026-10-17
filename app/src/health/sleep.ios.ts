// Apple Health. Asleep stages count; "in bed" counts only on nights with no
// asleep samples (older watches and manual entries record just that).

import {
  CategoryValueSleepAnalysis,
  isHealthDataAvailableAsync,
  queryCategorySamples,
  requestAuthorization,
} from '@kingstinct/react-native-healthkit';

import { trace } from '@/debug/trace';
import type { SleepSpan } from '@/ui/health-sleep';

export const HEALTH_STORE: string | null = 'Apple Health';

const SLEEP = 'HKCategoryTypeIdentifierSleepAnalysis' as const;

export async function healthAvailable(): Promise<boolean> {
  return isHealthDataAvailableAsync().catch(() => false);
}

/**
 * HealthKit never says whether read access was granted (that would leak
 * whether there is data), so true means only that the request went through.
 */
export async function requestSleepAccess(): Promise<boolean> {
  if (!(await healthAvailable())) return false;
  try {
    return await requestAuthorization({ toRead: [SLEEP] });
  } catch (e) {
    trace('health access failed', e);
    return false;
  }
}

export async function sleepSpans(fromMs: number, toMs: number): Promise<SleepSpan[]> {
  if (!(await healthAvailable())) return [];
  try {
    const samples = await queryCategorySamples(SLEEP, {
      limit: 0,
      filter: { date: { startDate: new Date(fromMs), endDate: new Date(toMs) } },
    });
    const asleep: SleepSpan[] = [];
    const inBed: SleepSpan[] = [];
    for (const s of samples) {
      const span = { startMs: s.startDate.getTime(), endMs: s.endDate.getTime() };
      if (s.value === CategoryValueSleepAnalysis.inBed) inBed.push(span);
      else if (s.value !== CategoryValueSleepAnalysis.awake) asleep.push(span);
    }
    return asleep.length > 0 ? asleep : inBed;
  } catch (e) {
    trace('health read failed', e);
    return [];
  }
}
