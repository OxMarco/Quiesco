// Demo mode: a pretend unit with three generated nights, so the app can be
// explored without hardware (App Review, the simulator, the curious). Shipped
// in release builds; the Welcome screen offers it as a secondary action. The
// unit is labelled as a demo everywhere and is removed like a real one
// (Settings → Remove demo data). It has no radio behind it, so screens check
// isDemo() rather than offering to connect.

import type { LogRecord } from '@/protocol/log';

import { saveRecords, setCursor, upsertUnit } from './db';

export const SAMPLE_SERIAL = 'DEMO000000000000';
export const SAMPLE_NAME = 'Demo unit';

/** True for the pretend unit loadSampleData creates. */
export function isDemo(unit: { serial: string } | null | undefined): boolean {
  return unit?.serial === SAMPLE_SERIAL;
}

export async function loadSampleData() {
  await upsertUnit({ serial: SAMPLE_SERIAL, peripheralId: 'demo', name: SAMPLE_NAME, firmware: '0.0.0-demo' });
  const records: LogRecord[] = [];
  const now = Math.floor(Date.now() / 1000);
  const step = 300;
  let seq = 1;
  for (let t = now - 3 * 86400; t <= now; t += step) {
    const d = new Date(t * 1000);
    const hour = d.getHours() + d.getMinutes() / 60;
    const asleep = hour >= 23 || hour < 7;
    const night = Math.floor((t - 12 * 3600) / 86400) % 3; // three different nights
    const wave = Math.sin(t / 1700) + Math.sin(t / 530) * 0.4;
    // Door closed overnight: CO2 builds up; the second night nobody opens a window.
    const sinceBed = asleep ? ((hour + 1) % 24) : 0;
    const co2 = asleep ? 520 + sinceBed * (night === 1 ? 110 : 60) + wave * 20 : 460 + wave * 25;
    records.push({
      sequence: seq++,
      validMask: 0x7b,
      epochS: t,
      msSinceBoot: (t - (now - 3 * 86400)) * 1000,
      bootCounter: 1,
      temperatureC: (asleep ? 19.6 : 22) + wave * 0.4 + (night === 2 ? 1.5 : 0),
      humidityPct: 47 + wave * 3 + (asleep ? 6 : 0),
      pressurePa: null,
      co2Ppm: Math.round(co2),
      lux: asleep ? 0.4 + Math.max(0, wave) : hour > 8 && hour < 19 ? 300 + wave * 120 : 40,
      noiseDb: (asleep ? 30 : 42) + Math.abs(wave) * 4 + (night === 0 && hour > 2 && hour < 2.3 ? 30 : 0),
      batteryV: 3.9,
    });
  }
  await saveRecords(SAMPLE_SERIAL, records);
  await setCursor(SAMPLE_SERIAL, seq, Date.now());
}
