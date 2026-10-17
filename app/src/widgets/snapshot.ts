// What the home-screen widgets show, as plain data both platforms render: the
// iOS widget (SwiftUI, targets/widget) reads it as JSON from the App Group,
// the Android widget (RoomWidget) as props. Same words as the Room tab.

import type { Measurement } from '@/protocol/codec';
import { Severity } from '@/protocol/comfort';
import { issueHeadline, issuesOf } from '@/ui/advice';
import { formatValue } from '@/ui/metrics';
import type { JudgeMode } from '@/ui/sleep';
import type { TempUnit } from '@/ui/units';

export type WidgetTone = 'ok' | 'warn' | 'bad' | 'none';

export interface WidgetSnapshot {
  /** The unit's name. */
  unit: string;
  /** When the reading was taken, Unix ms; 0 when there is none. */
  measuredAt: number;
  /** "Open a window", "All comfortable", or why there is nothing to show. */
  headline: string;
  tone: WidgetTone;
  co2: string;
  temperature: string;
  humidity: string;
  noise: string;
  /** Every value with its unit, for screen readers and the small widget's second line. */
  summary: string;
}

const TONE: Record<Severity, WidgetTone> = { [Severity.Ok]: 'ok', [Severity.Warn]: 'warn', [Severity.Bad]: 'bad' };

export function buildSnapshot(input: {
  unit: string | null;
  reading: Measurement | null;
  measuredAt: number | null;
  mode: JudgeMode;
  tempUnit: TempUnit;
}): WidgetSnapshot {
  const { unit, reading, measuredAt, mode, tempUnit } = input;
  if (!unit) return empty('Quiesco', 'Add your Quiesco in the app');
  if (!reading || !measuredAt) return empty(unit, 'No readings yet');
  const issue = issuesOf(reading, mode)[0] ?? null;
  const v = {
    co2: formatValue('co2', reading.co2Ppm, false),
    temperature: formatValue('temperature', reading.temperatureC, true, tempUnit),
    humidity: formatValue('humidity', reading.humidityPct, true),
    noise: formatValue('noise', reading.noiseDb, true),
  };
  return {
    unit,
    measuredAt,
    headline: issue ? issueHeadline(issue, mode) : 'All comfortable',
    tone: issue ? TONE[issue.severity] : 'ok',
    ...v,
    summary: `CO₂ ${v.co2} ppm · ${v.temperature} · ${v.humidity} · ${v.noise}`,
  };
}

function empty(unit: string, headline: string): WidgetSnapshot {
  return { unit, measuredAt: 0, headline, tone: 'none', co2: '--', temperature: '--', humidity: '--', noise: '--', summary: headline };
}
