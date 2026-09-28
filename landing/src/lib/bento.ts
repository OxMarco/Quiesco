// The unit's default screen, drawn like firmware/src/ui/Renderer.cpp drawBento():
// a CO₂ hero over a 2 × 2 grid on the 152 × 152 panel. A tile outside its comfort
// band (firmware/UI.md §3) inverts to light on black.

import { formatTemp } from './units';

export type Metric = 'co2' | 'temp' | 'hum' | 'noise';

export interface Scene {
  co2: number;
  temp: number;
  hum: number;
  noise: number;
  lux: number;
}

export const SCENES = {
  stuffy: { label: 'Stuffy air', reading: { co2: 1340, temp: 21.2, hum: 55, noise: 29, lux: 0 } },
  heat: { label: 'Heatwave', reading: { co2: 710, temp: 33, hum: 52, noise: 36, lux: 1 } },
  street: { label: 'Busy street', reading: { co2: 650, temp: 20.8, hum: 44, noise: 64, lux: 3 } },
} satisfies Record<string, { label: string; reading: Scene }>;

export type SceneId = keyof typeof SCENES;

const NO_LOW = -1e9;
const BANDS: Record<Metric, { warnLo: number; okLo: number; okHi: number; warnHi: number }> = {
  co2: { warnLo: NO_LOW, okLo: NO_LOW, okHi: 800, warnHi: 1200 },
  temp: { warnLo: 18, okLo: 20, okHi: 26, warnHi: 28 },
  hum: { warnLo: 25, okLo: 30, okHi: 60, warnHi: 70 },
  noise: { warnLo: NO_LOW, okLo: NO_LOW, okHi: 55, warnHi: 70 },
};

// [warn above, bad above, warn below, bad below]
const NUDGE: Record<Metric, [string, string, string, string]> = {
  co2: ['getting stuffy', 'open a window', 'getting stuffy', 'open a window'],
  temp: ['a bit warm', 'too hot', 'a bit chilly', 'too cold'],
  hum: ['a bit damp', 'too humid', 'a bit dry', 'too dry'],
  noise: ['a bit loud', 'too loud', 'a bit loud', 'too loud'],
};

const NAME: Record<Metric, string> = { co2: 'CO₂', temp: 'Temperature', hum: 'Humidity', noise: 'Noise' };

/** 0 comfortable, 1 warn, 2 bad. Rounded first, so the band matches the printed number. */
export function judge(metric: Metric, value: number): 0 | 1 | 2 {
  const b = BANDS[metric];
  const v = Math.round(value);
  if (v < b.warnLo || v > b.warnHi) return 2;
  if (v < b.okLo || v > b.okHi) return 1;
  return 0;
}

const INK = '#1E2B2D';
const PAPER = '#EEE8DC';

export function bentoSvg(s: Scene, fahrenheit: boolean): string {
  const tiles = [
    { m: 'co2', label: 'CO2 PPM', x: 0, y: 0, w: 152, h: 60, val: String(Math.round(s.co2)), size: 46, vb: 41, lb: 57 },
    { m: 'temp', label: 'TEMP', x: 0, y: 60, w: 76, h: 46, val: `${formatTemp(s.temp, fahrenheit)}°${fahrenheit ? 'F' : 'C'}`, size: 22, vb: 22, lb: 40 },
    { m: 'hum', label: 'HUMID', x: 76, y: 60, w: 76, h: 46, val: `${Math.round(s.hum)}%`, size: 22, vb: 22, lb: 40 },
    { m: 'lux', label: 'LIGHT', x: 0, y: 106, w: 76, h: 46, val: `${Math.round(s.lux)} lx`, size: 22, vb: 22, lb: 40 },
    { m: 'noise', label: 'NOISE', x: 76, y: 106, w: 76, h: 46, val: `${Math.round(s.noise)} dB`, size: 22, vb: 22, lb: 40 },
  ] as const;
  const font = `font-family="Fredoka, sans-serif" text-anchor="middle"`;
  const parts = tiles.map((t) => {
    const inverted = t.m !== 'lux' && judge(t.m, s[t.m]) > 0;
    const fg = inverted ? PAPER : INK;
    const cx = t.x + t.w / 2;
    return (
      (inverted ? `<rect x="${t.x}" y="${t.y}" width="${t.w}" height="${t.h}" fill="${INK}"/>` : '') +
      `<text x="${cx}" y="${t.y + t.vb}" ${font} fill="${fg}" font-weight="600" font-size="${t.size}"${t.m === 'co2' ? ' letter-spacing="-1"' : ''}>${t.val}</text>` +
      `<text x="${cx}" y="${t.y + t.lb}" ${font} fill="${fg}" font-weight="500" font-size="11" letter-spacing=".4">${t.label}</text>`
    );
  });
  parts.push(`<path d="M0 60.5H152M0 106.5H152M76.5 60V152" stroke="${INK}" stroke-width="1" fill="none"/>`);
  return parts.join('');
}

/** The line under the unit: the worst reading and what the panel would say about it. */
export function readoutHtml(s: Scene): string {
  let worst: Metric | null = null;
  let sev = 0;
  for (const m of ['co2', 'temp', 'hum', 'noise'] as const) {
    const j = judge(m, s[m]);
    if (j > sev) {
      sev = j;
      worst = m;
    }
  }
  if (!worst) return '';
  const above = worst === 'co2' || worst === 'noise' || Math.round(s[worst]) > BANDS[worst].okHi;
  const word = sev === 2 ? '<span class="flag bad">Bad</span>' : '<span class="flag warn">Warn</span>';
  return `<span>${word} ${NAME[worst]} is out of range: ${NUDGE[worst][(above ? 0 : 2) + (sev - 1)]}</span>`;
}
