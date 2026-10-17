// Battery life by measurement interval: a model from datasheet figures, not a
// measurement (README roadmap, "Measured power budget and battery life").
// Replace the constants with bench numbers once the cell has been run down.

import { isValidInterval, type IntervalSeconds } from '@/protocol/codec';

/** 400 mAh nominal; LiPos give ~85% before the cut-off. */
const USABLE_MAH = 340;

/**
 * Charge per measurement cycle, in mA·s:
 * - SCD41, two single shots (one discarded): ~90 mA·s each (datasheet
 *   0.45 mA average at one shot / 5 min, less 0.15 mA idle, × 300 s);
 * - nRF52840 awake ~11 s polling the mic and sensors at ~4 mA: ~45 mA·s;
 * - mic, BME280, VEML7700, flash write, an e-ink partial refresh: ~15 mA·s.
 */
const CYCLE_MAS = 240;

/** Between cycles: system-on sleep with BLE advertising, charger and dividers. */
const SLEEP_MA = 0.05;

export function batteryDays(intervalSeconds: number): number {
  const mAhPerDay = ((86_400 / intervalSeconds) * CYCLE_MAS) / 3600 + SLEEP_MA * 24;
  return USABLE_MAH / mAhPerDay;
}

/** Rounded down, so the label never promises more than the model. */
export function batteryLabel(intervalSeconds: number): string {
  const days = batteryDays(intervalSeconds);
  if (days < 14) return `~${Math.floor(days)} days`;
  if (days < 30) return `~${Math.floor(days / 7)} weeks`;
  const months = Math.floor(days / 30);
  return months === 1 ? '~1 month' : `~${months} months`;
}

/** What Settings offers. A unit already on 30 min (an older app, the bench script) keeps it listed. */
export function intervalChoices(current: number | undefined): IntervalSeconds[] {
  const offered: IntervalSeconds[] = [60, 300, 600];
  return current !== undefined && isValidInterval(current) && !offered.includes(current) ? [...offered, current] : offered;
}
