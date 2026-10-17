import { prose } from '@/ui/format';
import { EVIDENCE, evidenceScale, feelsLikeLabel, scaleParts } from '@/ui/advice';
import { formatValue } from '@/ui/metrics';
import { deltaFromUnit, deltaToUnit, formatTemp, fromUnit, toUnit } from '@/ui/units';

describe('temperature units', () => {
  test('conversions round-trip', () => {
    expect(toUnit(20, 'F')).toBe(68);
    expect(fromUnit(68, 'F')).toBe(20);
    expect(deltaToUnit(1, 'F')).toBeCloseTo(1.8);
    expect(deltaFromUnit(1.8, 'F')).toBeCloseTo(1);
    expect(toUnit(21.5, 'C')).toBe(21.5);
  });

  test('formatting: one decimal in °C, whole degrees in °F', () => {
    expect(formatTemp(27.44, 'C')).toBe('27.4°C');
    expect(formatTemp(27.44, 'F')).toBe('81°F');
    expect(formatValue('temperature', 27.44, true, 'F')).toBe('81°F');
    expect(formatValue('temperature', 27.44, false)).toBe('27.4');
  });

  test('feels like and the evidence scale follow the unit', () => {
    expect(feelsLikeLabel(27.4, 62, 'F')).toMatch(/^Feels 8\d°$/);
    const { scale, unit } = evidenceScale('temperature', 'F');
    expect(unit).toBe('°F');
    expect(scale.min).toBeCloseTo(57.2);
    expect(scaleParts(scale).map((p) => p.share)).toEqual(scaleParts(EVIDENCE.temperature.scale).map((p) => p.share).map((s) => expect.closeTo(s, 6)));
  });
});

describe('prose', () => {
  test('a lone sentence has no full stop, several are punctuated', () => {
    expect(prose('Best for sleep is 16 – 20 °C')).toBe('Best for sleep is 16 – 20 °C');
    expect(prose('Best for sleep is 40 – 60 %', false, 'The room passed it at about 21:10')).toBe(
      'Best for sleep is 40 – 60 %. The room passed it at about 21:10.',
    );
  });
});
