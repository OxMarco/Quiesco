// The unit's charge as the panel draws it (firmware Renderer.cpp): an outline
// filled in 5 % steps, with a bolt while charging. No percentage: while
// charging the voltage reads the charger's output and overstates the charge.

import Svg, { Path, Rect } from 'react-native-svg';

import { batteryPercent } from '@/protocol/comfort';
import { useTheme } from '@/theme';

export function BatteryIcon({ volts, charging, width = 30 }: { volts: number; charging: boolean; width?: number }) {
  const t = useTheme();
  const level = Math.round(batteryPercent(volts) / 5) * 5;
  const fillW = (28 * level) / 100;
  return (
    <Svg
      width={width}
      height={width / 2}
      viewBox="0 0 40 20"
      accessible
      accessibilityLabel={`Battery ${levelWords(level)}${charging ? ', charging' : ''}`}
    >
      <Rect x={36} y={6} width={3} height={8} rx={1.5} fill={t.text} />
      <Rect x={1} y={1} width={34} height={18} rx={4} fill="none" stroke={t.text} strokeWidth={2} />
      {fillW > 0 && <Rect x={4} y={4} width={fillW} height={12} rx={1.5} fill={t.text} />}
      {charging && (
        // Outlined so it stays visible across the fill edge, as on the panel.
        <Path d="M20 2.5L12.5 11H17.5L15.5 17.5L23.5 9H18.5Z" fill={t.surface} stroke={t.text} strokeWidth={1.5} strokeLinejoin="round" />
      )}
    </Svg>
  );
}

function levelWords(percent: number): string {
  if (percent >= 90) return 'full';
  if (percent >= 60) return 'mostly full';
  if (percent >= 35) return 'about half full';
  if (percent >= 10) return 'low';
  return 'empty';
}
