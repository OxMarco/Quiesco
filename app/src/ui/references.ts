// The one set of numbers the app's health claims use, on the Room tab's metric
// pages and on the Nights tab alike. Each is either a reference for the night
// (what sleep research counts as good for a bedroom while you sleep) or a
// general reference (a standard or guideline that is not about sleep alone).
// The panel's all-day comfort bands (protocol/comfort.ts) mirror the firmware
// and stay separate.
//
// - Temperature (night): 16 – 20 °C is the range sleep bodies recommend (Sleep
//   Foundation, Cleveland Clinic); sleep efficiency falls above 25 °C
//   (Baniassadi et al., Sci. Total Environ. 2023).
// - Humidity (night): 40 – 60 % keeps airways moist and mites and mould down;
//   under 30 % dries the airways, over 70 % favours mould (US EPA, Sleep
//   Foundation).
// - Light (night): at most 1 lux while asleep, 10 lux only if you must see
//   (Brown et al., PLOS Biology 2022).
// - Noise (general): single sounds under 45 dB indoors, an average under
//   30 dB (WHO night noise guidelines); over 55 dB is the panel's own "a bit
//   loud".
// - CO₂ (general): stuffy over 800 ppm, bad over 1,000. Sleep studies see no
//   effect under about 800 and worse sleep from a night average of 1,000
//   (Akimoto et al. 2025, ASHRAE 1837-RP; Kang et al., Build. Environ. 2024);
//   1,000 is also Health Canada's residential limit. See advice.ts.

import { roundHalfAway, Severity } from '@/protocol/comfort';
import type { MetricKey } from '@/protocol/codec';

export type ReferenceKind = 'night' | 'general';

export interface Reference {
  kind: ReferenceKind;
  warnLo: number;
  okLo: number;
  okHi: number;
  warnHi: number;
}

export const NOISE_NIGHT_AVG_DB = 30;
export const NOISE_NIGHT_PEAK_DB = 45;

export const REFERENCES: Record<MetricKey, Reference> = {
  noise: { kind: 'general', warnLo: -Infinity, okLo: -Infinity, okHi: NOISE_NIGHT_PEAK_DB, warnHi: 55 },
  co2: { kind: 'general', warnLo: -Infinity, okLo: -Infinity, okHi: 800, warnHi: 1000 },
  temperature: { kind: 'night', warnLo: 14, okLo: 16, okHi: 20, warnHi: 24 },
  humidity: { kind: 'night', warnLo: 30, okLo: 40, okHi: 60, warnHi: 70 },
  light: { kind: 'night', warnLo: -Infinity, okLo: -Infinity, okHi: 1, warnHi: 10 },
};

export const REFERENCE_LABEL: Record<ReferenceKind, string> = {
  night: 'Reference for the night',
  general: 'General reference',
};

/** Rounded before judging, like the panel, so the band matches the printed number. */
export function referenceSeverity(metric: MetricKey, value: number): Severity {
  const b = REFERENCES[metric];
  const v = roundHalfAway(value);
  if (v < b.warnLo || v > b.warnHi) return Severity.Bad;
  if (v < b.okLo || v > b.okHi) return Severity.Warn;
  return Severity.Ok;
}
