// One metric up close: the last hours over its comfort band, which way it is
// moving, and a way into the evidence behind the range.

import { router, useLocalSearchParams } from 'expo-router';
import { useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';

import { useSession } from '@/ble/session';
import { openEvidence } from '@/components/evidence';
import { Face } from '@/components/face';
import { MetricChart } from '@/components/metric-chart';
import { NotFound, SEVERITY_CLASS, Sheet, T } from '@/components/ui';
import type * as db from '@/data/db';
import type { MetricKey } from '@/protocol/codec';
import { metricValue } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import {
  bandSentence,
  crossedAbove,
  DAY_NOISE,
  EVIDENCE,
  hourDelta,
  type JudgedMetric,
  judgeFor,
  stateWord,
  trendSentence,
  upperLimit,
} from '@/ui/advice';
import { REFERENCES } from '@/ui/references';
import { formatValue, METRICS } from '@/ui/metrics';
import { formatTime, judgeMode, sleepPhase, VERDICT_LEAD_MIN } from '@/ui/sleep';
import { RECENT_S, useReading } from '@/ui/use-reading';

export default function MetricScreen() {
  const { metric } = useLocalSearchParams<{ metric: string }>();
  const unit = useSession((s) => s.units[0] ?? null);
  if (!(metric in METRICS)) return <NotFound />;
  // Forgotten while the sheet was open, or opened by a link with no unit.
  if (!unit) return <NotFound message="There is no unit on this phone." />;
  return <MetricPage unit={unit} metric={metric as MetricKey} />;
}

function MetricPage({ unit, metric }: { unit: db.Unit; metric: MetricKey }) {
  const t = useTheme();
  const tempUnit = useSession((s) => s.tempUnit);
  const sleepWindow = useSession((s) => s.sleepWindow);
  const { reading, measuredAt, recent, live } = useReading(unit);
  // Frozen at open: the page is short-lived, and a ticking clock would redraw the chart.
  const [now] = useState(() => Date.now());
  const nowS = Math.floor(now / 1000);
  const mode = judgeMode(now, sleepWindow);
  const meta = METRICS[metric];

  const value = reading ? metricValue(reading, metric) : null;
  const severity = judgeFor(metric, value, mode);
  const word = stateWord(metric, value, severity, mode);

  // The chart runs up to bedtime when it is close, so the mark has somewhere to sit.
  const phase = sleepPhase(now, sleepWindow);
  const bedSoon = live && phase.kind === 'before' && phase.inMin <= VERDICT_LEAD_MIN;
  // Live, the chart ends now; otherwise at the newest reading on the phone, so
  // hours out of range do not leave it empty.
  const endS = live || measuredAt === null ? nowS : Math.min(nowS, Math.floor(measuredAt / 1000));
  const fromS = endS - RECENT_S;
  const toS = bedSoon ? phase.fromS : endS;
  const points = recent.filter((p) => p.t >= fromS && p.t <= toS);
  const hasPoints = points.length > 0;

  const delta = hourDelta(metric, value, recent, (measuredAt ?? now) / 1000);
  const trend = trendSentence(metric, delta, tempUnit);
  const limit = upperLimit(metric, mode);
  const since = limit !== null && value !== null && value > limit ? crossedAbove(metric, points, limit) : null;
  const band = bandSentence(metric, mode, tempUnit);
  const evidence = metric === 'light' ? null : EVIDENCE[metric as JudgedMetric];

  return (
    <Sheet>
    <ScrollView contentContainerClassName="px-4 pt-6 pb-12 gap-4">
      <View className="flex-row justify-between items-center gap-4">
        <T className="font-title text-2xl shrink">{metric === 'co2' ? 'Air freshness' : meta.label}</T>
        <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">Close</T>
        </Pressable>
      </View>

      <View className="flex-row justify-between items-end px-1">
        <T className="font-number text-[48px] leading-[54px]">
          {formatValue(metric, value, false, tempUnit)}
          <T className="font-body-medium text-base text-muted">
            {' '}
            {metric === 'temperature' ? (tempUnit === 'F' ? '°F' : '°C') : meta.unit}
          </T>
        </T>
        <View className="flex-row items-center gap-1.5 pb-2.5">
          {severity !== null && <Face size={22} severity={severity} bare ink={severityColor(t, severity)} />}
          <T className={`font-body-semi text-[15px] ${severity === null ? 'text-muted' : SEVERITY_CLASS[severity].text}`}>{word.charAt(0).toUpperCase() + word.slice(1)}</T>
        </View>
      </View>

      <View className="bg-surface rounded-card px-3 pt-4 pb-2">
        {hasPoints ? (
          <MetricChart
            metric={metric}
            points={points}
            fromS={fromS}
            toS={toS}
            showBand={metric !== 'light' && limit !== null && (mode === 'sleep' || metric === 'co2')}
            lines={
              metric === 'noise' && mode === 'day'
                ? [
                    {
                      value: DAY_NOISE.warnHi,
                      color: t.warn,
                      label: `${DAY_NOISE.warnHi} dB(A)`,
                    },
                  ]
                : metric === 'light'
                  ? [
                      {
                        value: REFERENCES.light.warnHi,
                        color: t.bad,
                        label: 'too bright',
                      },
                    ]
                  : []
            }
            marks={bedSoon ? [{ s: phase.fromS, label: `bed ${formatTime(phase.fromS)}` }] : []}
            tempUnit={tempUnit}
          />
        ) : (
          <T className="text-sm leading-5 text-muted px-1 pb-2">
            Nothing from the unit’s log in the last 3 hours. Pull down on the Room tab to sync it.
          </T>
        )}
      </View>

      <View className="gap-1 px-1">
        {trend && <T className="font-body-semi text-base">{trend}</T>}
        {!trend && hasPoints && delta !== null && <T className="font-body-semi text-base">Steady over the last hour.</T>}
        <T className="text-[15px] leading-[22px] text-muted">
          {band}
          {since !== null && since > fromS ? ` The room passed it at about ${formatTime(since)}.` : ''}
        </T>
      </View>

      {evidence ? (
        <Pressable
          onPress={() => openEvidence(metric as JudgedMetric)}
          accessibilityRole="button"
          className="bg-surface rounded-card px-4 py-4 flex-row items-center justify-between mt-2 active:opacity-70">
          <T className="font-body-medium text-base">Why this range?</T>
          <T className="text-muted text-lg">›</T>
        </Pressable>
      ) : (
        <T className="text-[13px] leading-[18px] text-muted px-1">
          Moonlight is under 1 lux; a dim night light 5 – 10 lux. Light at night delays sleep (Brown et al., PLOS Biology, 2022).
        </T>
      )}
    </ScrollView>
    </Sheet>
  );
}
