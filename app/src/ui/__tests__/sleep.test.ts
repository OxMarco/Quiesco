import type { StoredPoint } from '@/data/db';
import { Severity, verdict } from '@/protocol/comfort';
import { EVIDENCE, feelsLike, issuesOf, scaleParts } from '@/ui/advice';
import { analyzeNight } from '@/ui/nights';
import {
  DEFAULT_SLEEP_WINDOW,
  nightOf,
  type SleepWindow,
  sleepPhase,
  typicalLightsOut,
  validTimes,
  windowForNight,
} from '@/ui/sleep';

const at = (y: number, m: number, d: number, h: number, min = 0) => new Date(y, m - 1, d, h, min).getTime() / 1000;

const point = (t: number, patch: Partial<StoredPoint> = {}): StoredPoint => ({
  t,
  temperature: 22,
  humidity: 45,
  co2: 600,
  lux: 0,
  noise: 30,
  ...patch,
});

describe('sleep window', () => {
  const w: SleepWindow = { weekday: { bedMin: 23 * 60 + 30, wakeMin: 7 * 60 }, weekend: null, set: true };

  test('an evening bedtime runs into the next morning', () => {
    // 2026-09-23 is a Wednesday.
    expect(windowForNight('2026-09-23', w)).toEqual({ fromS: at(2026, 9, 23, 23, 30), toS: at(2026, 9, 24, 7) });
  });

  test('a bedtime after midnight belongs to the evening before', () => {
    const late: SleepWindow = { ...w, weekday: { bedMin: 60, wakeMin: 9 * 60 } };
    expect(windowForNight('2026-09-23', late)).toEqual({ fromS: at(2026, 9, 24, 1), toS: at(2026, 9, 24, 9) });
    expect(nightOf(at(2026, 9, 24, 1) * 1000)).toBe('2026-09-23');
  });

  test('Friday and Saturday use the weekend times', () => {
    const weekend: SleepWindow = { ...w, weekend: { bedMin: 0, wakeMin: 9 * 60 } };
    expect(windowForNight('2026-09-25', weekend).fromS).toBe(at(2026, 9, 26, 0)); // Friday
    expect(windowForNight('2026-09-26', weekend).fromS).toBe(at(2026, 9, 27, 0)); // Saturday
    expect(windowForNight('2026-09-27', weekend).fromS).toBe(at(2026, 9, 27, 23, 30)); // Sunday
  });

  test('windows must last 3 to 14 hours', () => {
    expect(validTimes(DEFAULT_SLEEP_WINDOW.weekday)).toBe(true);
    expect(validTimes({ bedMin: 23 * 60, wakeMin: 23 * 60 + 30 })).toBe(false);
    expect(validTimes({ bedMin: 21 * 60, wakeMin: 11 * 60 })).toBe(true);
    expect(validTimes({ bedMin: 21 * 60, wakeMin: 11 * 60 + 15 })).toBe(false);
  });

  test('phase: before, during, and the next night after waking', () => {
    const before = sleepPhase(at(2026, 9, 23, 22, 40) * 1000, w);
    expect(before).toMatchObject({ kind: 'before', inMin: 50 });
    expect(sleepPhase(at(2026, 9, 24, 3) * 1000, w).kind).toBe('during');
    const after = sleepPhase(at(2026, 9, 24, 8) * 1000, w);
    expect(after).toMatchObject({ kind: 'before', fromS: at(2026, 9, 24, 23, 30) });
  });

  test('lights-out suggestion is the median of weekday nights', () => {
    const nights = ['2026-09-20', '2026-09-21', '2026-09-22', '2026-09-23'];
    const points: StoredPoint[] = [];
    nights.forEach((night, i) => {
      const [y, m, d] = night.split('-').map(Number);
      const dark = at(y, m, d, 23, 50 + (i % 2) * 5);
      for (let t = dark - 3600; t <= dark + 3600; t += 300) points.push(point(t, { lux: t < dark ? 80 : 0.5 }));
    });
    expect(typicalLightsOut(points, nights, w)).toBe(0); // 23:55 rounds to 00:00
    expect(typicalLightsOut(points, nights.slice(0, 3), w)).toBeNull();
  });
});

describe('night story', () => {
  const window = { fromS: at(2026, 9, 23, 23, 30), toS: at(2026, 9, 24, 7) };

  test('only readings inside the window count', () => {
    const points = [
      point(at(2026, 9, 23, 22, 30), { noise: 58 }), // TV before bed
      point(at(2026, 9, 23, 22, 35), { noise: 58 }),
      point(at(2026, 9, 24, 4, 10), { noise: 51 }),
      point(at(2026, 9, 24, 4, 25), { noise: 49 }),
      point(at(2026, 9, 24, 5), { noise: 45 }), // at the limit, not above it
    ];
    const story = analyzeNight(points, window);
    expect(story.samples).toBe(3);
    expect(story.noise.spells).toEqual([
      { fromS: at(2026, 9, 24, 4, 10), toS: at(2026, 9, 24, 5), above: true, severity: Severity.Warn },
    ]);
  });

  test('a single loud reading is not a spell', () => {
    const points = [point(at(2026, 9, 24, 1), { noise: 30 }), point(at(2026, 9, 24, 1, 5), { noise: 70 }), point(at(2026, 9, 24, 1, 10), { noise: 30 })];
    expect(analyzeNight(points, window).noise.spells).toEqual([]);
  });

  test('noise average is an energy average, not arithmetic', () => {
    const story = analyzeNight([point(window.fromS, { noise: 30 }), point(window.fromS + 300, { noise: 50 })], window);
    expect(story.noise.average).toBeCloseTo(47, 0);
  });

  test('CO2 turns stuffy at the first reading above the band', () => {
    const points = [
      point(at(2026, 9, 24, 1), { co2: 790 }),
      point(at(2026, 9, 24, 2, 30), { co2: 830 }),
      point(at(2026, 9, 24, 6), { co2: 1400 }),
    ];
    const { co2 } = analyzeNight(points, window);
    expect(co2.spells.map((sp) => [sp.fromS, sp.toS])).toEqual([[at(2026, 9, 24, 2, 30), null]]);
    expect(co2.peak?.v).toBe(1400);
  });
});

describe('advice', () => {
  // Comfortable by the night references: 16 – 20 °C, 40 – 60 %.
  const base = {
    temperatureC: 19,
    humidityPct: 45,
    pressurePa: null,
    co2Ppm: 600,
    lux: 3,
    noiseDb: 30,
    batteryV: 3.9,
    validMask: 0x7b,
  };

  test('issues come worst first, ties in the panel order', () => {
    // Judged by the references (references.ts), so noise over 55 dB at night is
    // as bad as a 29 °C room; CO₂ at 900 ppm only warns.
    const m = { ...base, co2Ppm: 900, temperatureC: 29, noiseDb: 60 };
    const issues = issuesOf(m);
    expect(issues.map((i) => i.metric)).toEqual(['temperature', 'noise', 'co2']);
    expect(issues[0].metric).toBe(verdict(m).metric);
    expect(issuesOf(base)).toEqual([]);
  });

  test('feels like follows the heat index and ignores humidity in cool rooms', () => {
    expect(feelsLike(18, 90)).toBe(18);
    expect(feelsLike(27.4, 62)).toBeGreaterThan(28);
    expect(feelsLike(27.4, 62)).toBeLessThan(29.5);
    expect(feelsLike(32, 70)).toBeGreaterThan(38);
  });

  test('scale zones fill the bar exactly', () => {
    for (const e of Object.values(EVIDENCE)) {
      const total = scaleParts(e.scale).reduce((sum, p) => sum + p.share, 0);
      expect(total).toBeCloseTo(1, 6);
    }
  });
});
