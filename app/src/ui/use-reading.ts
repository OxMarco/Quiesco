// The unit's current reading as the Room tab and the metric page show it: the
// live one while connected, otherwise the newest logged one, plus the last few
// hours of the log for trends and charts.

import { useEffect, useState } from 'react';

import { link } from '@/ble/link';
import { useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import * as db from '@/data/db';
import { trace } from '@/debug/trace';
import type { Measurement } from '@/protocol/codec';

/** How much of the log the Room tab keeps at hand. */
export const RECENT_S = 3 * 3600;

export function useReading(unit: db.Unit) {
  const state = useStore(link, (s) => s);
  const dataVersion = useSession((s) => s.dataVersion);
  const [stored, setStored] = useState<db.StoredPoint | null>(null);
  const [recent, setRecent] = useState<db.StoredPoint[]>([]);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      const nowS = Math.floor(Date.now() / 1000);
      const latest = await db.latestPoint(unit.serial);
      // Out of range for hours, "the last few hours" means the last few the
      // phone has, not an empty stretch up to now.
      const endS = Math.min(nowS, latest?.t ?? nowS);
      // A little past now, so a clock that runs ahead cannot hide the newest points.
      const points = await db.recordsBetween(unit.serial, endS - RECENT_S, nowS + 3600);
      if (cancelled) return;
      setStored(latest);
      setRecent(points);
    })().catch((e) => trace('reading load failed', e));
    return () => {
      cancelled = true;
    };
  }, [unit.serial, dataVersion]);

  const connected = state.phase === 'ready' && state.serial === unit.serial;
  const live = connected && state.reading && state.reading.validMask !== 0 ? state.reading : null;
  const reading: Measurement | null = live ?? (stored ? fromPoint(stored) : null);
  const measuredAt = live ? state.readingAt : stored ? stored.t * 1000 : null;
  const chargingV = connected && state.status?.charging && !state.info?.noBattery ? state.reading?.batteryV : null;

  return { state, connected, live: live !== null, reading, measuredAt, recent, chargingV };
}

export function fromPoint(p: db.StoredPoint): Measurement {
  return {
    temperatureC: p.temperature,
    humidityPct: p.humidity,
    pressurePa: null,
    co2Ppm: p.co2,
    lux: p.lux,
    noiseDb: p.noise,
    batteryV: null,
    validMask: 0,
  };
}
