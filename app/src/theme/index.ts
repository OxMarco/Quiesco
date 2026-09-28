// Brand "Tide & Sand" (app/brand). Day follows system light mode, Night dark.
//
// This is the one palette source. NativeWind classes read it through CSS
// variables (tailwind.config.js, applied by ThemeRoot); SVG drawing and the
// navigation theme read the hex values directly through useTheme().
//
// Verdict colours are reserved for comfort verdicts: comfortable is the tide
// primary itself (the device "speaks only when wrong"); warn and bad always
// appear with an icon and a word. Driftwood is brand-only, never beside data.

import { useColorScheme } from 'react-native';
import { vars } from 'nativewind';

import { Severity } from '@/protocol/comfort';

const day = {
  bg: '#EEE8DC', // Sand
  surface: '#F8F4EC', // Shell
  raised: '#E4DDCE',
  line: '#D8D0C0',
  text: '#1E2B2D', // Deep water
  textMuted: '#5E6B6B', // Wet stone
  primary: '#2F6468', // Tide
  onPrimary: '#F8F4EC',
  warn: '#A87A00',
  bad: '#C83A62',
  driftwood: '#A85A2E',
};

type Palette = typeof day;

const night: Palette = {
  bg: '#0F1718', // Night sea
  surface: '#182324', // Kelp
  raised: '#213033',
  line: '#2A3A3C',
  text: '#E8E4DA', // Foam
  textMuted: '#9AA5A4', // Pebble
  primary: '#7DB3B3', // Shallows
  onPrimary: '#0F1718',
  warn: '#B5880C',
  bad: '#E35D82',
  driftwood: '#E8A77F',
};

/** The e-ink panel: warm paper and ink in both themes, like the real glass. */
export const Panel = { paper: '#EEE8DC', ink: '#1E2B2D' } as const;

export type Theme = Palette & { dark: boolean };

export function useTheme(): Theme {
  const dark = useColorScheme() === 'dark';
  return { ...(dark ? night : day), dark };
}

const triplet = (hex: string) =>
  [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)).join(' ');

const kebab = (key: string) => key.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`);

function toVars(p: Palette) {
  return vars(Object.fromEntries(Object.entries(p).map(([k, v]) => [`--${kebab(k)}`, triplet(v)])));
}

/** NativeWind variable sets, applied at the root by ThemeRoot. */
export const themeVars = { day: toVars(day), night: toVars(night) };

export function severityColor(t: Theme, s: Severity | null): string {
  if (s === Severity.Bad) return t.bad;
  if (s === Severity.Warn) return t.warn;
  return t.primary;
}

/** Hex with alpha, for SVG fills. */
export function withAlpha(hex: string, alpha: number): string {
  return `${hex}${Math.round(alpha * 255).toString(16).padStart(2, '0')}`;
}
