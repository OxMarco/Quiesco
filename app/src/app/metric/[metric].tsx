// One metric up close: the last hours over its comfort band, which way it is
// moving, and the evidence behind the range.

import { router, useLocalSearchParams } from 'expo-router';
import { useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';

import { useSession } from '@/ble/session';
import { EvidenceTag } from '@/components/evidence';
import { Face } from '@/components/face';
import { MetricChart } from '@/components/metric-chart';
import { SEVERITY_CLASS, T } from '@/components/ui';
import type * as db from '@/data/db';
import type { MetricKey } from '@/protocol/codec';
import { metricValue, Severity } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import {
  bandSentence,
  crossedAbove,
  DAY_NOISE,
  EVIDENCE,
  hourDelta,
  type JudgedMetric,
  judgeFor,
  referenceLabel,
  trendSentence,
  upperLimit,
} from '@/ui/advice';
import { REFERENCE_LABEL, REFERENCES } from '@/ui/references';
import { ago } from '@/ui/format';
import { formatValue, METRICS } from '@/ui/metrics';
import { formatTime, type JudgeMode, judgeMode, sleepPhase, VERDICT_LEAD_MIN } from '@/ui/sleep';
import { RECENT_S, useReading } from '@/ui/use-reading';

export default function MetricScreen() {
  const { metric } = useLocalSearchParams<{ metric: string }>();
  const unit = useSession((s) => s.units[0] ?? null);
  if (!unit || !(metric in METRICS)) return null;
  return <MetricPage unit={unit} metric={metric as MetricKey} />;
}

/** The state word under the value: the verdict where there is one, a plain description otherwise. */
function stateWord(metric: MetricKey, value: number | null, severity: Severity | null, mode: JudgeMode): string {
  if (value === null) return 'no reading';
  if (metric === 'light') return value < 3 ? 'dark' : value < 50 ? 'dim' : 'lit';
  if (severity === null) return mode === 'day' ? 'not judged by day' : 'not judged';
  if (severity === Severity.Ok) return metric === 'noise' ? 'quiet' : metric === 'co2' ? 'fresh' : 'comfortable';
  const limit = upperLimit(metric, mode);
  const above = limit !== null && value > limit;
  switch (metric) {
    case 'co2':
      return severity === Severity.Bad ? 'open a window' : 'stuffy';
    case 'temperature':
      return above ? (severity === Severity.Bad ? 'too hot' : 'warm') : severity === Severity.Bad ? 'too cold' : 'cool';
    case 'humidity':
      return above ? (severity === Severity.Bad ? 'too humid' : 'damp') : severity === Severity.Bad ? 'too dry' : 'dry';
    case 'noise':
      return severity === Severity.Bad ? (mode === 'day' ? 'very loud' : 'too loud') : 'loud';
  }
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
    <ScrollView className="flex-1 bg-bg" contentContainerClassName="px-5 pt-6 pb-12 gap-4">
      <View className="flex-row justify-between items-center gap-4">
        <T className="font-title text-2xl shrink">{metric === 'co2' ? 'CO₂ · Air freshness' : meta.label}</T>
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
          <T className={`font-body-semi text-[13px] ${severity === null ? 'text-muted' : SEVERITY_CLASS[severity].text}`}>{word}</T>
        </View>
      </View>

      <View className="bg-surface rounded-card p-4 gap-3">
        <T className="font-body-semi text-xs uppercase tracking-widest text-muted">
          {endS === nowS ? 'Last 3 hours' : `3 hours to ${formatTime(endS)}`}
        </T>
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
                      label: `${DAY_NOISE.warnHi} dB`,
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
          <T className="text-sm leading-5 text-muted">
            Nothing from the unit’s log in the last 3 hours. Pull down on the Room tab to sync it.
          </T>
        )}

        <View className="gap-0.5">
          {trend && <T className="font-body-semi text-sm">{trend}</T>}
          {!trend && hasPoints && delta !== null && <T className="font-body-semi text-sm">Steady over the last hour.</T>}
          <T className="text-[13px] leading-[18px] text-muted">
            {/* By day noise is judged for hearing, which is not one of the references. */}
            {!(metric === 'noise' && mode === 'day') && <T className="font-body-semi text-ink">{referenceLabel(metric)}: </T>}
            {band}
            {since !== null && since > fromS ? ` The room passed it at about ${formatTime(since)}.` : ''}
          </T>
        </View>
        <T className="text-xs text-muted">
          {live ? 'Live' : 'Last synced reading'} · measured {ago(measuredAt, now)} · history synced {ago(unit.lastSyncMs, now)}
        </T>
      </View>

      {evidence ? (
        <View className="bg-surface rounded-card p-4 gap-2.5">
          <View className="flex-row items-center gap-2">
            <EvidenceTag metric={metric as JudgedMetric} />
            <T className="text-[12px] text-muted">{REFERENCE_LABEL[evidence.kind]}</T>
          </View>
          <T className="text-[14px] leading-[20px]">{evidence.body}</T>
          <T className="text-[13px] leading-[18px] text-muted">{evidence.reference}</T>
          {metric === 'noise' && mode === 'day' && (
            <T className="text-[13px] leading-[18px] text-muted">
              By day Quiesco judges noise for hearing instead: the EPA’s 70 dB daily average and NIOSH’s 85 dB working limit.
            </T>
          )}
          <T className="text-[11px] leading-[15px] text-muted">Sources: {evidence.sources}</T>
        </View>
      ) : (
        <View className="bg-surface rounded-card p-4 gap-2">
          <T className="text-[12px] text-muted">{REFERENCE_LABEL[REFERENCES.light.kind]}</T>
          <T className="text-[14px] leading-[20px]">
            Light at night tells your body it is still day and delays sleep. Keep the bedroom at or below {REFERENCES.light.okHi} lux while
            you sleep, and under {REFERENCES.light.warnHi} lux if you need to see.
          </T>
          <T className="text-[13px] leading-[18px] text-muted">For reference: moonlight is under 1 lux, a dim night light 5 – 10 lux.</T>
          <T className="text-[11px] leading-[15px] text-muted">Sources: Brown et al., PLOS Biology (2022).</T>
        </View>
      )}
    </ScrollView>
  );
}
