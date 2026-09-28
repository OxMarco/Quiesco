// Picks the unit's e-ink screen by showing it: a small drawing of each layout
// in panel pixels (152 × 152), after firmware/UI.md §4. Every thumbnail shows
// the same sample room, a bit stuffy and a bit damp, so the three compare.

import { Pressable, View } from 'react-native';
import Svg, { Circle, G, Line, Path, Rect, Text as SvgText } from 'react-native-svg';

import { DisplayScreen } from '@/protocol/codec';
import { Panel, useTheme } from '@/theme';

import { T } from './ui';

const P = Panel.paper;
const I = Panel.ink;
const NUM = 'Fredoka_600SemiBold';
const REG = 'Fredoka_500Medium';

function FaceScreen() {
  return (
    <G>
      <Circle cx={76} cy={64} r={40} fill="none" stroke={I} strokeWidth={5} />
      <Circle cx={62} cy={56} r={4} fill={I} />
      <Circle cx={90} cy={56} r={4} fill={I} />
      <Line x1={64} y1={80} x2={88} y2={80} stroke={I} strokeWidth={5} strokeLinecap="round" />
      <SvgText x={76} y={126} textAnchor="middle" fontFamily={NUM} fontSize={17} fill={I}>
        CO2 1050 ppm
      </SvgText>
      <SvgText x={76} y={145} textAnchor="middle" fontFamily={REG} fontSize={12} fill={I}>
        getting stuffy
      </SvgText>
    </G>
  );
}

// Scale position (0–1) of value and comfort band, per UI.md's ledger scales.
const LEDGER = [
  { label: 'CO2', value: '1050 ppm', at: 0.41, band: [0, 0.25] },
  { label: 'Temp', value: '24°C', at: 0.56, band: [0.4, 0.64] },
  { label: 'Humidity', value: '63%', at: 0.63, band: [0.3, 0.6] },
  { label: 'Noise', value: '38 dB', at: 0.11, band: [0, 0.36] },
  { label: 'Light', value: '4 lx', at: 0.15, band: null },
] as const;

function LedgerScreen() {
  const x = (f: number) => 10 + f * 132;
  return (
    <G>
      {LEDGER.map((r, i) => {
        const y = 23 + i * 28;
        return (
          <G key={r.label}>
            <SvgText x={10} y={y} fontFamily={REG} fontSize={10} fill={I}>
              {r.label}
            </SvgText>
            <SvgText x={142} y={y} textAnchor="end" fontFamily={NUM} fontSize={10} fill={I}>
              {r.value}
            </SvgText>
            <Line x1={10} y1={y + 9} x2={142} y2={y + 9} stroke={I} strokeWidth={1} strokeDasharray="1 2" />
            {r.band && <Line x1={x(r.band[0])} y1={y + 9} x2={x(r.band[1])} y2={y + 9} stroke={I} strokeWidth={3} />}
            <Circle cx={x(r.at)} cy={y + 9} r={4} fill={I} />
            <Circle cx={x(r.at)} cy={y + 9} r={2} fill={P} />
          </G>
        );
      })}
    </G>
  );
}

function Tile({ x, y, value, label, inverted }: { x: number; y: number; value: string; label: string; inverted?: boolean }) {
  const fg = inverted ? P : I;
  return (
    <G>
      {inverted && <Rect x={x} y={y} width={76} height={46} fill={I} />}
      <SvgText x={x + 38} y={y + 24} textAnchor="middle" fontFamily={NUM} fontSize={17} fill={fg}>
        {value}
      </SvgText>
      <SvgText x={x + 38} y={y + 40} textAnchor="middle" fontFamily={REG} fontSize={11} fill={fg}>
        {label}
      </SvgText>
    </G>
  );
}

function BentoScreen() {
  return (
    <G>
      <Rect x={0} y={0} width={152} height={60} fill={I} />
      <SvgText x={76} y={43} textAnchor="middle" fontFamily={NUM} fontSize={38} fill={P}>
        1050
      </SvgText>
      <SvgText x={76} y={56} textAnchor="middle" fontFamily={REG} fontSize={10} fill={P}>
        CO2 PPM
      </SvgText>
      <Tile x={0} y={60} value="24°C" label="Temp" />
      <Tile x={76} y={60} value="63%" label="Humidity" inverted />
      <Tile x={0} y={106} value="4 lx" label="Light" />
      <Tile x={76} y={106} value="38 dB" label="Noise" />
      <Path d="M0 106H152M76 60V152" stroke={I} strokeWidth={1} />
    </G>
  );
}

const OPTIONS = [
  { value: DisplayScreen.Face, label: 'Face', Screen: FaceScreen },
  { value: DisplayScreen.Ledger, label: 'Ledger', Screen: LedgerScreen },
  { value: DisplayScreen.Bento, label: 'Bento', Screen: BentoScreen },
];

export function ScreenPicker({
  value,
  onChange,
  disabled,
}: {
  value: DisplayScreen | null;
  onChange: (v: DisplayScreen) => void;
  disabled?: boolean;
}) {
  const t = useTheme();
  return (
    <View className={`flex-row gap-3 ${disabled ? 'opacity-40' : ''}`} accessibilityRole="radiogroup">
      {OPTIONS.map(({ value: v, label, Screen }) => {
        const on = v === value;
        return (
          <Pressable
            key={v}
            disabled={disabled}
            onPress={() => onChange(v)}
            accessibilityRole="radio"
            accessibilityLabel={label}
            accessibilityState={{ checked: on }}
            className="flex-1 items-center gap-1.5 active:opacity-60">
            <View
              style={{ borderColor: on ? t.primary : 'transparent', borderWidth: 2, borderRadius: 14, padding: 3, width: '100%' }}>
              <View style={{ borderRadius: 10, overflow: 'hidden', aspectRatio: 1 }}>
                <Svg width="100%" height="100%" viewBox="0 0 152 152">
                  <Rect width={152} height={152} fill={P} />
                  <Screen />
                </Svg>
              </View>
            </View>
            <T className={`text-[13px] ${on ? 'font-body-semi' : 'text-muted'}`}>{label}</T>
          </Pressable>
        );
      })}
    </View>
  );
}
