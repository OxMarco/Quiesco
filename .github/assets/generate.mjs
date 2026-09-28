// Draws the README artwork (banner and screens, light and dark) from the brand
// palette and the landing page's self-hosted fonts.
// Run from the repository root after `npm install` in landing/:
//   node .github/assets/generate.mjs
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';

const ROOT = new URL('../..', import.meta.url).pathname.replace(/\/$/, '');
const OUT = `${ROOT}/.github/assets`;
mkdirSync(OUT, { recursive: true });

const FONTS = `${ROOT}/landing/node_modules/@fontsource`;
const face = (family, file, weight) =>
  `@font-face{font-family:'${family}';font-weight:${weight};src:url(data:font/woff2;base64,${readFileSync(`${FONTS}/${file}`).toString('base64')}) format('woff2')}`;
const fontCss = (list) => `<style>${list.join('')}</style>`;
const FREDOKA = [
  face('Fredoka', 'fredoka/files/fredoka-latin-500-normal.woff2', 500),
  face('Fredoka', 'fredoka/files/fredoka-latin-600-normal.woff2', 600),
];
const KIWI = [face('Kiwi Maru', 'kiwi-maru/files/kiwi-maru-latin-500-normal.woff2', 500)];
const NOTO = [
  face('Noto Sans', 'noto-sans/files/noto-sans-latin-400-normal.woff2', 400),
  face('Noto Sans', 'noto-sans/files/noto-sans-latin-600-normal.woff2', 600),
];

const THEMES = {
  light: { bg: '#EEE8DC', surface: '#F8F4EC', line: '#D8D0C0', ink: '#1E2B2D', muted: '#5E6B6B', tide: '#2F6468', chassis: '#F8F4EC', edge: '#D8D0C0', shadow: 'rgba(30,43,45,0.28)' },
  dark: { bg: '#0F1718', surface: '#182324', line: '#2A3A3C', ink: '#E8E4DA', muted: '#9AA5A4', tide: '#7DB3B3', chassis: '#213033', edge: '#2A3A3C', shadow: 'rgba(0,0,0,0.6)' },
};
const INK = '#1E2B2D';
const PAPER = '#EEE8DC';
const RF = `font-family="Fredoka, 'Arial Rounded MT Bold', sans-serif"`;

// ---- Panels (152 × 152, drawn like firmware/src/ui/Renderer.cpp) ----------

const BANDS = {
  co2: [-1e9, -1e9, 800, 1200],
  temp: [18, 20, 26, 28],
  hum: [25, 30, 60, 70],
  noise: [-1e9, -1e9, 55, 70],
};
const judge = (m, v) => {
  const [wl, ol, oh, wh] = BANDS[m];
  v = Math.round(v);
  return v < wl || v > wh ? 2 : v < ol || v > oh ? 1 : 0;
};

function bento(s) {
  const tiles = [
    { m: 'co2', label: 'CO2 PPM', x: 0, y: 0, w: 152, h: 60, val: String(s.co2), size: 46, vb: 41, lb: 57 },
    { m: 'temp', label: 'TEMP', x: 0, y: 60, w: 76, h: 46, val: `${s.temp.toFixed(1)}°C`, size: 22, vb: 22, lb: 40 },
    { m: 'hum', label: 'HUMID', x: 76, y: 60, w: 76, h: 46, val: `${s.hum}%`, size: 22, vb: 22, lb: 40 },
    { m: 'lux', label: 'LIGHT', x: 0, y: 106, w: 76, h: 46, val: `${s.lux} lx`, size: 22, vb: 22, lb: 40 },
    { m: 'noise', label: 'NOISE', x: 76, y: 106, w: 76, h: 46, val: `${s.noise} dB`, size: 22, vb: 22, lb: 40 },
  ];
  const parts = tiles.map((t) => {
    const inv = t.m !== 'lux' && judge(t.m, s[t.m]) > 0;
    const fg = inv ? PAPER : INK;
    const cx = t.x + t.w / 2;
    return (
      (inv ? `<rect x="${t.x}" y="${t.y}" width="${t.w}" height="${t.h}" fill="${INK}"/>` : '') +
      `<text x="${cx}" y="${t.y + t.vb}" ${RF} text-anchor="middle" fill="${fg}" font-weight="600" font-size="${t.size}"${t.m === 'co2' ? ' letter-spacing="-1"' : ''}>${t.val}</text>` +
      `<text x="${cx}" y="${t.y + t.lb}" ${RF} text-anchor="middle" fill="${fg}" font-weight="500" font-size="11" letter-spacing=".4">${t.label}</text>`
    );
  });
  parts.push(`<path d="M0 60.5H152M0 106.5H152M76.5 60V152" stroke="${INK}" stroke-width="1" fill="none"/>`);
  return parts.join('');
}

function faceScreen() {
  // Worst metric is warn: flat mouth, callout and nudge.
  return (
    `<circle cx="76" cy="64" r="40" fill="none" stroke="${INK}" stroke-width="5"/>` +
    `<circle cx="62" cy="56" r="4" fill="${INK}"/><circle cx="90" cy="56" r="4" fill="${INK}"/>` +
    `<path d="M62 80H90" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>` +
    `<text x="76" y="126" ${RF} text-anchor="middle" fill="${INK}"><tspan font-size="11" font-weight="500">CO2 </tspan><tspan font-size="21" font-weight="600">1080</tspan><tspan font-size="11" font-weight="500"> ppm</tspan></text>` +
    `<text x="76" y="145" ${RF} text-anchor="middle" fill="${INK}" font-size="12" font-weight="500">getting stuffy</text>`
  );
}

function ledgerScreen() {
  const x0 = 10, x1 = 142;
  const lin = (lo, hi) => (v) => x0 + ((Math.min(Math.max(v, lo), hi) - lo) / (hi - lo)) * (x1 - x0);
  const log = (v) => x0 + (Math.log10(Math.min(Math.max(v, 1), 10000)) / 4) * (x1 - x0);
  const rows = [
    { l: 'CO2', v: '1080 ppm', s: lin(400, 2000), val: 1080, band: [400, 800] },
    { l: 'TEMP', v: '21.4°C', s: lin(10, 35), val: 21.4, band: [20, 26] },
    { l: 'HUMID', v: '48%', s: lin(0, 100), val: 48, band: [30, 60] },
    { l: 'NOISE', v: '32 dB', s: lin(30, 100), val: 32, band: [30, 55] },
    { l: 'LIGHT', v: '2 lx', s: log, val: 2, band: null },
  ];
  return rows
    .map((r, i) => {
      const y = 23 + i * 28;
      const g = y + 9;
      let out =
        `<text x="10" y="${y}" ${RF} fill="${INK}" font-size="10" font-weight="500">${r.l}</text>` +
        `<text x="142" y="${y}" ${RF} text-anchor="end" fill="${INK}" font-size="10" font-weight="600">${r.v}</text>` +
        `<path d="M${x0} ${g}H${x1}" stroke="${INK}" stroke-width="1" stroke-dasharray="1 2"/>`;
      if (r.band) out += `<path d="M${r.s(r.band[0])} ${g}H${r.s(r.band[1])}" stroke="${INK}" stroke-width="3"/>`;
      const mx = r.s(r.val);
      out += `<circle cx="${mx}" cy="${g}" r="4" fill="${INK}"/><circle cx="${mx}" cy="${g}" r="2" fill="${PAPER}"/>`;
      return out;
    })
    .join('');
}

// A unit: chassis with a bezel around the 152 × 152 glass, top-left at (x, y), glass scaled by k.
function unit(t, x, y, k, screen, id) {
  const glass = 152 * k;
  const bezel = Math.round(glass * 0.13);
  const r = 14 * k * 0.9;
  const size = glass + 2 * bezel;
  return `
  <g transform="translate(${x} ${y})">
    <rect x="0" y="0" width="${size}" height="${size}" rx="${r + bezel}" fill="${t.chassis}" stroke="${t.edge}" stroke-width="1.5" filter="url(#shadow)"/>
    <clipPath id="glass-${id}"><rect x="${bezel}" y="${bezel}" width="${glass}" height="${glass}" rx="${r}"/></clipPath>
    <g clip-path="url(#glass-${id})">
      <rect x="${bezel}" y="${bezel}" width="${glass}" height="${glass}" fill="${PAPER}"/>
      <g transform="translate(${bezel} ${bezel}) scale(${k})">${screen}</g>
    </g>
    <rect x="${bezel}" y="${bezel}" width="${glass}" height="${glass}" rx="${r}" fill="none" stroke="${INK}" stroke-opacity=".12"/>
  </g>`;
}

const shadowDef = (t) =>
  `<filter id="shadow" x="-30%" y="-30%" width="160%" height="170%"><feDropShadow dx="0" dy="22" stdDeviation="22" flood-color="${t.shadow}"/></filter>`;

// ---- Banner -----------------------------------------------------------------

function banner(name) {
  const t = THEMES[name];
  const W = 1280, H = 480;
  const chips = ['CO₂', 'Temperature', 'Humidity', 'Light', 'Noise'];
  let cx = 96;
  const chipSvg = chips
    .map((c) => {
      const w = 22 + c.length * 10.2;
      const s = `<rect x="${cx}" y="330" width="${w}" height="38" rx="19" fill="none" stroke="${t.tide}" stroke-width="1.5"/><text x="${cx + w / 2}" y="355" text-anchor="middle" font-family="'Noto Sans', system-ui, sans-serif" font-size="16" font-weight="600" fill="${t.tide}">${c}</text>`;
      cx += w + 10;
      return s;
    })
    .join('');
  const k = 1.72;
  const unitSize = 152 * k * 1.26;
  const scene = { co2: 1340, temp: 21.2, hum: 55, noise: 29, lux: 0 };
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" role="img" aria-label="Quiesco: an open-source bedroom environment monitor">
  ${fontCss([...FREDOKA, ...KIWI, ...NOTO])}
  <defs>
    ${shadowDef(t)}
    <radialGradient id="glow" cx="0.78" cy="0.5" r="0.5"><stop offset="0" stop-color="${t.tide}" stop-opacity="${name === 'dark' ? 0.22 : 0.14}"/><stop offset="1" stop-color="${t.tide}" stop-opacity="0"/></radialGradient>
  </defs>
  <rect width="${W}" height="${H}" rx="28" fill="${t.bg}"/>
  <rect width="${W}" height="${H}" rx="28" fill="url(#glow)"/>
  <g fill="none" stroke="${t.tide}" stroke-opacity="${name === 'dark' ? 0.18 : 0.14}" stroke-width="2" stroke-linecap="round">
    <path d="M0 420q80 -26 160 0t160 0t160 0t160 0t160 0t160 0t160 0t160 0"/>
    <path d="M0 450q80 -26 160 0t160 0t160 0t160 0t160 0t160 0t160 0t160 0"/>
  </g>
  <text x="96" y="104" font-family="'Noto Sans', system-ui, sans-serif" font-size="16" font-weight="600" letter-spacing="2.4" fill="${t.muted}">OPEN-SOURCE BEDROOM ENVIRONMENT MONITOR</text>
  <text x="92" y="196" font-family="'Kiwi Maru', Georgia, serif" font-size="92" font-weight="500" fill="${t.ink}">Quiesco</text>
  <text x="96" y="254" font-family="'Noto Sans', system-ui, sans-serif" font-size="27" fill="${t.ink}">Measure your bedroom. <tspan fill="${t.tide}" font-weight="600">Sleep better in it.</tspan></text>
  <text x="96" y="296" font-family="'Noto Sans', system-ui, sans-serif" font-size="18" fill="${t.muted}">Hardware, firmware and app — all GPLv3, no cloud, no account.</text>
  ${chipSvg}
  ${unit(t, W - 96 - unitSize, (H - unitSize) / 2 - 8, k, bento(scene), 'b')}
</svg>`;
}

// ---- Screens strip ------------------------------------------------------------

function screens(name) {
  const t = THEMES[name];
  const W = 1280, H = 520;
  const k = 1.6;
  const size = 152 * k * 1.26;
  const gap = (W - 3 * size) / 4;
  const items = [
    { title: 'Face', sub: 'speaks only when something is wrong', svg: faceScreen() },
    { title: 'Ledger', sub: 'every reading against its comfort band', svg: ledgerScreen() },
    { title: 'Bento', sub: 'big numerals, out-of-band tiles invert', svg: bento({ co2: 1340, temp: 21.2, hum: 55, noise: 29, lux: 0 }) },
  ];
  const body = items
    .map((it, i) => {
      const x = gap + i * (size + gap);
      return (
        unit(t, x, 40, k, it.svg, `s${i}`) +
        `<text x="${x + size / 2}" y="${40 + size + 62}" text-anchor="middle" font-family="'Kiwi Maru', Georgia, serif" font-size="30" font-weight="500" fill="${t.ink}">${it.title}</text>` +
        `<text x="${x + size / 2}" y="${40 + size + 92}" text-anchor="middle" font-family="'Noto Sans', system-ui, sans-serif" font-size="17" fill="${t.muted}">${it.sub}</text>`
      );
    })
    .join('');
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" role="img" aria-label="The three e-ink screens: Face, Ledger and Bento">
  ${fontCss([...FREDOKA, ...KIWI, ...NOTO])}
  <defs>${shadowDef(t)}</defs>
  <rect width="${W}" height="${H}" rx="28" fill="${t.bg}"/>
  ${body}
</svg>`;
}

for (const n of ['light', 'dark']) {
  writeFileSync(`${OUT}/banner-${n}.svg`, banner(n));
  writeFileSync(`${OUT}/screens-${n}.svg`, screens(n));
}
console.log('ok');
