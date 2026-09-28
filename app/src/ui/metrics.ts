// How each metric is named, formatted and scaled. Formats follow the e-ink
// bento tiles (UI.md §4) so a number reads the same on phone and panel.

import type { MetricKey } from '@/protocol/codec';
import { roundHalfAway } from '@/protocol/comfort';
import type { StoredPoint } from '@/data/db';

import { REFERENCES } from './references';
import { formatTemp, type TempUnit } from './units';

export interface MetricMeta {
  key: MetricKey;
  label: string;
  unit: string;
  /** SF Symbol / Material Symbol names for the metric's icon. */
  sf: string;
  md: string;
  /** Chart scale, from the ledger gauges. Light is drawn on a log scale. */
  min: number;
  max: number;
  log?: boolean;
  /** The comfortable band to shade on charts, or null for light. */
  band: [number, number] | null;
}

export const METRICS: Record<MetricKey, MetricMeta> = {
  co2: {
    key: 'co2',
    label: 'CO₂',
    unit: 'ppm',
    sf: 'wind',
    md: 'air',
    min: 400,
    max: 2000,
    band: [400, REFERENCES.co2.okHi],
  },
  temperature: {
    key: 'temperature',
    label: 'Temperature',
    unit: '°C',
    sf: 'thermometer.medium',
    md: 'thermostat',
    min: 10,
    max: 35,
    band: [REFERENCES.temperature.okLo, REFERENCES.temperature.okHi],
  },
  humidity: {
    key: 'humidity',
    label: 'Humidity',
    unit: '%',
    sf: 'humidity',
    md: 'humidity_percentage',
    min: 0,
    max: 100,
    band: [REFERENCES.humidity.okLo, REFERENCES.humidity.okHi],
  },
  noise: {
    key: 'noise',
    label: 'Noise',
    unit: 'dB',
    sf: 'waveform',
    md: 'graphic_eq',
    min: 30,
    max: 100,
    band: [30, REFERENCES.noise.okHi],
  },
  light: {
    key: 'light',
    label: 'Light',
    unit: 'lx',
    sf: 'sun.max',
    md: 'light_mode',
    min: 1,
    max: 10000,
    log: true,
    band: null,
  },
};

export const METRIC_ORDER: MetricKey[] = ['co2', 'temperature', 'humidity', 'noise', 'light'];

/** Rounded the way the panel rounds, with the tile's unit. */
export function formatValue(metric: MetricKey, value: number | null, withUnit = true, tempUnit: TempUnit = 'C'): string {
  if (value === null) return '--';
  if (metric === 'temperature') return formatTemp(value, tempUnit, withUnit);
  const unit = METRICS[metric].unit;
  let text: string;
  if (metric === 'light' && value >= 999.5) text = `${roundHalfAway(value / 1000)}k`;
  else text = `${roundHalfAway(value)}`;
  if (!withUnit) return text;
  return metric === 'humidity' ? `${text}${unit}` : `${text} ${unit}`;
}

export function pointValue(p: StoredPoint, metric: MetricKey): number | null {
  switch (metric) {
    case 'co2':
      return p.co2;
    case 'temperature':
      return p.temperature;
    case 'humidity':
      return p.humidity;
    case 'noise':
      return p.noise;
    case 'light':
      return p.lux;
  }
}

/** Position of a value on the metric's chart scale, 0–1, clamped. */
export function scalePosition(metric: MetricKey, value: number): number {
  const m = METRICS[metric];
  const t = m.log
    ? (Math.log10(Math.max(value, m.min)) - Math.log10(m.min)) / (Math.log10(m.max) - Math.log10(m.min))
    : (value - m.min) / (m.max - m.min);
  return Math.min(1, Math.max(0, t));
}
