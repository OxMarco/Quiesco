// Health Connect: built into Android 14 and later, an app from the Play Store
// before that. Sleep sessions are read whole; their stages are not needed.

import { getGrantedPermissions, getSdkStatus, initialize, readRecords, requestPermission, SdkAvailabilityStatus } from 'react-native-health-connect';

import { trace } from '@/debug/trace';
import type { SleepSpan } from '@/ui/health-sleep';

export const HEALTH_STORE: string | null = 'Health Connect';

let ready: Promise<boolean> | null = null;

export function healthAvailable(): Promise<boolean> {
  ready ??= (async () => {
    try {
      if ((await getSdkStatus()) !== SdkAvailabilityStatus.SDK_AVAILABLE) return false;
      return await initialize();
    } catch (e) {
      trace('health connect unavailable', e);
      return false;
    }
  })();
  return ready;
}

async function canReadSleep(): Promise<boolean> {
  const granted = await getGrantedPermissions();
  return granted.some((p) => 'recordType' in p && p.recordType === 'SleepSession' && p.accessType === 'read');
}

export async function requestSleepAccess(): Promise<boolean> {
  if (!(await healthAvailable())) return false;
  try {
    if (await canReadSleep()) return true;
    await requestPermission([{ accessType: 'read', recordType: 'SleepSession' }]);
    return await canReadSleep();
  } catch (e) {
    trace('health access failed', e);
    return false;
  }
}

export async function sleepSpans(fromMs: number, toMs: number): Promise<SleepSpan[]> {
  if (!(await healthAvailable())) return [];
  try {
    if (!(await canReadSleep())) return [];
    const spans: SleepSpan[] = [];
    let pageToken: string | undefined;
    do {
      const page = await readRecords('SleepSession', {
        timeRangeFilter: { operator: 'between', startTime: new Date(fromMs).toISOString(), endTime: new Date(toMs).toISOString() },
        pageToken,
      });
      for (const r of page.records) spans.push({ startMs: Date.parse(r.startTime), endMs: Date.parse(r.endTime) });
      pageToken = page.pageToken || undefined;
    } while (pageToken);
    return spans;
  } catch (e) {
    trace('health read failed', e);
    return [];
  }
}
