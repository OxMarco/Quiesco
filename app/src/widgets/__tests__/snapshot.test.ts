import type { Measurement } from '@/protocol/codec';
import { buildSnapshot } from '@/widgets/snapshot';

const reading = (patch: Partial<Measurement> = {}): Measurement => ({
  temperatureC: 19.5,
  humidityPct: 48,
  pressurePa: null,
  co2Ppm: 640,
  lux: 0.4,
  noiseDb: 31,
  batteryV: null,
  validMask: 0x7f,
  ...patch,
});

const base = { unit: 'Bedroom', measuredAt: 1_790_000_000_000, mode: 'sleep' as const, tempUnit: 'C' as const };

describe('widget snapshot', () => {
  test('no unit and no reading say so', () => {
    expect(buildSnapshot({ ...base, unit: null, reading: null }).headline).toBe('Add your Quiesco in the app');
    const none = buildSnapshot({ ...base, reading: null });
    expect(none).toMatchObject({ unit: 'Bedroom', headline: 'No readings yet', tone: 'none', co2: '--' });
  });

  test('a comfortable room', () => {
    expect(buildSnapshot({ ...base, reading: reading() })).toMatchObject({
      headline: 'All comfortable',
      tone: 'ok',
      co2: '640',
      temperature: '19.5°C',
      humidity: '48%',
      noise: '31 dB(A)',
    });
  });

  test('the first problem leads, with its tone', () => {
    const s = buildSnapshot({ ...base, reading: reading({ co2Ppm: 1400 }) });
    expect(s.tone).toBe('bad');
    expect(s.headline).toBe('Open a window');
  });

  test('temperature follows the unit setting', () => {
    expect(buildSnapshot({ ...base, tempUnit: 'F', reading: reading() }).temperature).toBe('67°F');
  });
});
