import type { StoredPoint } from '@/data/db';
import { Severity } from '@/protocol/comfort';
import { analyzeNight, spellReport } from '@/ui/nights';
import { formatTime } from '@/ui/sleep';

const at = (h: number, min = 0) => new Date(2026, 8, 27, h, min).getTime() / 1000;

const point = (t: number, patch: Partial<StoredPoint> = {}): StoredPoint => ({
  t,
  temperature: 18,
  humidity: 50,
  co2: 600,
  lux: 0,
  noise: 30,
  ...patch,
});

const window = { fromS: at(7), toS: at(12) };

describe('night spells', () => {
  test('a comfortable night is optimal', () => {
    const story = analyzeNight([point(at(8)), point(at(9))], window);
    expect(spellReport('temperature', story.temperature!.spells, window)).toEqual({ text: 'Optimal', others: 0, severity: Severity.Ok });
    expect(spellReport('humidity', story.humidity!.spells, window).text).toBe('Optimal');
    expect(spellReport('co2', story.co2.spells, window).text).toBe('Fresh all night');
    expect(spellReport('noise', story.noise.spells, window).text).toBe('Calm all night');
    expect(spellReport('light', story.light!.spells, window).text).toBe('Dark all night');
  });

  test('a spell that ends reads "from … to …", worst held level wins', () => {
    const story = analyzeNight(
      [
        point(at(7, 30)),
        point(at(8), { temperature: 22 }),
        point(at(9), { temperature: 26 }),
        point(at(9, 30), { temperature: 26 }),
        point(at(10, 20)),
        point(at(11)),
      ],
      window,
    );
    const r = spellReport('temperature', story.temperature!.spells, window);
    expect(r.text).toBe(`Too hot from ${formatTime(at(8))} to ${formatTime(at(10, 20))}`);
    expect(r.severity).toBe(Severity.Bad);
  });

  test('one extreme reading does not raise the severity', () => {
    const story = analyzeNight([point(at(8), { temperature: 22 }), point(at(9), { temperature: 30 }), point(at(10), { temperature: 22 })], window);
    expect(story.temperature!.spells[0].severity).toBe(Severity.Warn);
  });

  test('a single reading is neither a spell nor the end of one', () => {
    const story = analyzeNight(
      [
        point(at(8), { noise: 60 }), // one slam
        point(at(8, 5)),
        point(at(9), { noise: 50 }),
        point(at(9, 5), { noise: 50 }),
        point(at(9, 10)), // a lull, not the end
        point(at(9, 15), { noise: 50 }),
        point(at(9, 20), { noise: 50 }),
        point(at(9, 25)),
        point(at(9, 30)),
      ],
      window,
    );
    expect(story.noise.spells).toEqual([{ fromS: at(9), toS: at(9, 25), above: true, severity: Severity.Warn }]);
    expect(spellReport('noise', story.noise.spells, window).text).toBe(`A bit noisy from ${formatTime(at(9))} to ${formatTime(at(9, 25))}`);
  });

  test('a spell lasting to wake reads "from …"', () => {
    const story = analyzeNight([point(at(8)), point(at(8, 20), { co2: 900 }), point(at(11), { co2: 950 })], window);
    expect(spellReport('co2', story.co2.spells, window)).toEqual({
      text: `Stuffy from ${formatTime(at(8, 20))}`,
      others: 0,
      severity: Severity.Warn,
    });
  });

  test('a spell ending just before wake reads as lasting to it', () => {
    const story = analyzeNight(
      [point(at(10), { co2: 900 }), point(at(11), { co2: 900 }), point(at(11, 56)), point(at(11, 58))],
      window,
    );
    expect(spellReport('co2', story.co2.spells, window).text).toBe(`Stuffy from ${formatTime(at(10))}`);
  });

  test('the longest of equally bad spells is reported, the rest counted', () => {
    const story = analyzeNight(
      [
        point(at(8), { humidity: 35 }),
        point(at(8, 5), { humidity: 35 }),
        point(at(8, 10)),
        point(at(8, 15)),
        point(at(9), { humidity: 36 }),
        point(at(9, 10), { humidity: 37 }),
        point(at(10)),
        point(at(10, 5)),
      ],
      window,
    );
    const r = spellReport('humidity', story.humidity!.spells, window);
    expect(r.text).toBe(`A bit dry from ${formatTime(at(9))} to ${formatTime(at(10))}`);
    expect(r.others).toBe(1);
  });

  test('switching from too cold to too hot starts a new spell', () => {
    const story = analyzeNight(
      [
        point(at(8), { temperature: 12 }),
        point(at(8, 5), { temperature: 12 }),
        point(at(9), { temperature: 26 }),
        point(at(9, 5), { temperature: 26 }),
      ],
      window,
    );
    expect(story.temperature!.spells.map((s) => s.above)).toEqual([false, true]);
  });
});

describe('night bands', () => {
  test('light over 1 lux is dim, over 10 lux too bright', () => {
    const story = analyzeNight(
      [
        point(at(8), { lux: 0.4 }),
        point(at(9), { lux: 4 }),
        point(at(9, 30), { lux: 30 }),
        point(at(10), { lux: 30 }),
        point(at(11)),
        point(at(11, 5)),
      ],
      window,
    );
    expect(spellReport('light', story.light!.spells, window)).toEqual({
      text: `Too bright from ${formatTime(at(9))} to ${formatTime(at(11))}`,
      others: 0,
      severity: Severity.Bad,
    });
  });
});
