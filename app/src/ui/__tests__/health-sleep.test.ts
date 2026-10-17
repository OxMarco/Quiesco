import { mergeSpans, nightlySleep, sleepForNight, usualTimes } from '@/ui/health-sleep';

const at = (d: number, h: number, min = 0) => new Date(2026, 9, d, h, min).getTime();
const span = (d1: number, h1: number, m1: number, d2: number, h2: number, m2: number) => ({
  startMs: at(d1, h1, m1),
  endMs: at(d2, h2, m2),
});

describe('recorded sleep', () => {
  test('stages a few minutes apart are one sleep; a long gap is not', () => {
    const merged = mergeSpans([span(10, 23, 30, 11, 2, 0), span(11, 2, 20, 11, 7, 0), span(11, 14, 0, 11, 14, 40)]);
    expect(merged).toEqual([span(10, 23, 30, 11, 7, 0), span(11, 14, 0, 11, 14, 40)]);
  });

  test('a night is the longest sleep starting in it, and a nap is not one', () => {
    const nights = nightlySleep([span(10, 23, 30, 11, 7, 0), span(11, 14, 0, 11, 15, 30)]);
    expect([...nights.keys()]).toEqual(['2026-10-10']);
  });

  test('sleep after midnight still belongs to the evening before', () => {
    expect(sleepForNight('2026-10-10', [span(11, 0, 45, 11, 8, 0)])).toEqual({
      fromS: at(11, 0, 45) / 1000,
      toS: at(11, 8) / 1000,
    });
    expect(sleepForNight('2026-10-11', [span(11, 0, 45, 11, 8, 0)])).toBeNull();
  });

  test('usual times span midnight and round to the 15-minute step', () => {
    const spans = [
      span(10, 23, 40, 11, 7, 5),
      span(11, 23, 50, 12, 6, 55),
      span(13, 0, 10, 13, 7, 20),
      span(13, 23, 35, 14, 7, 0),
    ];
    expect(usualTimes(spans)).toEqual({ bedMin: 23 * 60 + 45, wakeMin: 7 * 60 });
  });

  test('too few nights give no usual times', () => {
    expect(usualTimes([span(10, 23, 0, 11, 7, 0), span(11, 23, 0, 12, 7, 0)])).toBeNull();
  });
});
