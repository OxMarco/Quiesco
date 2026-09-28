// The brand mark: the unit's e-ink face. As a logo it sleeps; as a verdict it
// wakes and shows the same three mouths as the firmware's face screen.

import Svg, { Path, Rect } from 'react-native-svg';

import { Severity } from '@/protocol/comfort';
import { Panel } from '@/theme';

interface FaceProps {
  size: number;
  /** Omit for the sleeping logo face. */
  severity?: Severity | null;
  /** Paper colour; defaults to the e-ink panel. */
  paper?: string;
  ink?: string;
  /** Draw only the face (no panel), e.g. inline beside text. */
  bare?: boolean;
}

export function Face({ size, severity, paper = Panel.paper, ink = Panel.ink, bare }: FaceProps) {
  const awake = severity !== undefined && severity !== null;
  const eyes = awake
    ? 'M37 46.5v1M63 46.5v1' // dots, drawn as round-capped strokes
    : 'M34 46q6 6 12 0M54 46q6 6 12 0';
  const mouth =
    severity === Severity.Bad
      ? 'M42 64q8 -6 16 0'
      : severity === Severity.Warn
        ? 'M42 61H58'
        : 'M42 60q8 6 16 0';
  return (
    <Svg width={size} height={size} viewBox={bare ? '21 21 58 58' : '0 0 100 100'}>
      {!bare && <Rect x={0} y={0} width={100} height={100} rx={26} fill={paper} />}
      <Path
        d={`${eyes}${mouth}`}
        fill="none"
        stroke={ink}
        strokeWidth={awake ? 7 : 5}
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </Svg>
  );
}

/** The app icon: the sleeping face on the tide ground. */
export function AppMark({ size }: { size: number }) {
  return (
    <Svg width={size} height={size} viewBox="0 0 100 100">
      <Rect width={100} height={100} rx={22.5} fill="#2F6468" />
      <Rect x={21} y={21} width={58} height={58} rx={14} fill={Panel.paper} />
      <Path d="M34 46q6 6 12 0M54 46q6 6 12 0M42 60q8 6 16 0" fill="none" stroke={Panel.ink} strokeWidth={5} strokeLinecap="round" />
    </Svg>
  );
}
