import type { StoredPoint } from '@/data/db';
import type { Measurement } from '@/protocol/codec';
import { Severity } from '@/protocol/comfort';
import { crossedAbove, hourDelta, issueHeadline, issuesOf, judgeFor, trendSentence } from '@/ui/advice';
import { formatDuration, judgeMode, type SleepWindow } from '@/ui/sleep';

const at = (y: number, m: number, d: number, h: number, min = 0) => new Date(y, m - 1, d, h, min).getTime();

const w: SleepWindow = { weekday: { bedMin: 23 * 60 + 15, wakeMin: 7 * 60 }, weekend: null, set: true };

const reading = (patch: Partial<Measurement> = {}): Measurement => ({
  temperatureC: 27,
  humidityPct: 55,
  pressurePa: null,
  co2Ppm: 700,
  lux: 300,
  noiseDb: 60,
  batteryV: null,
  validMask: 0x7b,
  ...patch,
});

describe('judge mode', () => {
  test('sleep verdicts start an hour before bedtime and last until wake', () => {
    expect(judgeMode(at(2026, 9, 28, 15, 20), w)).toBe('day');
    expect(judgeMode(at(2026, 9, 28, 22, 14), w)).toBe('day');
    expect(judgeMode(at(2026, 9, 28, 22, 15), w)).toBe('sleep');
    expect(judgeMode(at(2026, 9, 29, 3), w)).toBe('sleep');
    expect(judgeMode(at(2026, 9, 29, 7, 1), w)).toBe('day');
  });

  test('by day a warm, ordinarily noisy room is not an issue', () => {
    expect(issuesOf(reading(), 'day')).toEqual([]);
    expect(issuesOf(reading(), 'sleep').map((i) => i.metric)).toEqual(['temperature', 'noise']);
  });

  test('stale air is an issue at any hour', () => {
    expect(issuesOf(reading({ co2Ppm: 900 }), 'day')).toEqual([{ metric: 'co2', severity: Severity.Warn, above: true }]);
  });

  test('daytime noise is judged for hearing', () => {
    expect(judgeFor('noise', 69.4, 'day')).toBe(Severity.Ok);
    expect(judgeFor('noise', 70.5, 'day')).toBe(Severity.Warn);
    expect(judgeFor('noise', 86, 'day')).toBe(Severity.Bad);
    // At night the WHO reference: over 45 dB warns, over 55 dB is bad.
    expect(judgeFor('noise', 50, 'sleep')).toBe(Severity.Warn);
    expect(judgeFor('noise', 60, 'sleep')).toBe(Severity.Bad);
    expect(judgeFor('temperature', 30, 'day')).toBeNull();
    expect(issueHeadline({ metric: 'noise', severity: Severity.Bad, above: true }, 'day')).toBe('Very loud');
    expect(issueHeadline({ metric: 'noise', severity: Severity.Bad, above: true }, 'sleep')).toBe('Too loud to sleep');
  });
});

describe('trend', () => {
  const nowS = 1_800_000_000;
  const point = (t: number, co2: number): StoredPoint => ({ t, temperature: 22, humidity: 45, co2, lux: 0, noise: 30 });
  const points = [point(nowS - 7200, 600), point(nowS - 3600 - 300, 950), point(nowS - 1800, 1100), point(nowS - 60, 1250)];

  test('compares with the log point nearest an hour ago', () => {
    expect(hourDelta('co2', 1260, points, nowS)).toBe(310);
    expect(hourDelta('co2', 1260, [point(nowS - 7200, 600)], nowS)).toBeNull();
    expect(hourDelta('noise', 60, points, nowS)).toBeNull();
  });

  test('says only changes big enough to matter', () => {
    expect(trendSentence('co2', 310, 'C')).toBe('Up 310 ppm in the last hour.');
    expect(trendSentence('co2', -40, 'C')).toBeNull();
    expect(trendSentence('temperature', 1.4, 'C')).toBe('Up 1.5° in the last hour.');
    expect(trendSentence('temperature', -1, 'F')).toBe('Down 2° in the last hour.');
    expect(trendSentence('humidity', 4.2, 'C')).toBe('Up 4 % in the last hour.');
  });

  test('finds when the room last rose above the limit', () => {
    expect(crossedAbove('co2', points, 800)).toBe(nowS - 3600 - 300);
    expect(crossedAbove('co2', [...points, point(nowS, 700)], 800)).toBeNull();
  });
});

test('durations under an hour drop the hours', () => {
  expect(formatDuration(35)).toBe('35 min');
  expect(formatDuration(60)).toBe('1 h');
  expect(formatDuration(95)).toBe('1 h 35 min');
});
