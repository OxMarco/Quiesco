// Temperature unit. Everything is stored and judged in °C (as the unit sends
// it); °F is only a way of showing it.

export type TempUnit = 'C' | 'F';

/** Regions that use Fahrenheit for everyday temperatures. */
const FAHRENHEIT_REGIONS = ['US', 'LR', 'MM', 'BS', 'BZ', 'KY', 'PW', 'FM', 'MH'];

/** A first guess from the phone's locale, before the user has chosen. */
export function localeTempUnit(): TempUnit {
  const locale = Intl.DateTimeFormat().resolvedOptions().locale;
  const region = /[-_]([A-Z]{2})(?:$|[-_])/.exec(locale)?.[1];
  return region && FAHRENHEIT_REGIONS.includes(region) ? 'F' : 'C';
}

export function toUnit(c: number, unit: TempUnit): number {
  return unit === 'F' ? (c * 9) / 5 + 32 : c;
}

export function fromUnit(v: number, unit: TempUnit): number {
  return unit === 'F' ? ((v - 32) * 5) / 9 : v;
}

/** A difference (a calibration offset), not a temperature: no 32 °F shift. */
export function deltaToUnit(c: number, unit: TempUnit): number {
  return unit === 'F' ? (c * 9) / 5 : c;
}

export function deltaFromUnit(v: number, unit: TempUnit): number {
  return unit === 'F' ? (v * 5) / 9 : v;
}

export function tempSymbol(unit: TempUnit): string {
  return unit === 'F' ? '°F' : '°C';
}

/** One decimal in °C (the panel's precision); whole degrees in °F, whose steps are finer. */
export function formatTemp(c: number, unit: TempUnit, withUnit = true): string {
  const v = toUnit(c, unit);
  const text = unit === 'F' ? `${Math.round(v)}` : (Math.round(v * 10) / 10).toFixed(1);
  return withUnit ? `${text}${tempSymbol(unit)}` : text;
}
