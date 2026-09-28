import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { Pressable, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';

import type { LinkState } from '@/ble/link';
import { reconnect, reloadUnits, session, setTempUnit, syncNow, useSession } from '@/ble/session';
import { BatteryIcon } from '@/components/battery';
import { openEvidence } from '@/components/evidence';
import { AppMark, Face } from '@/components/face';
import { TempUnitPicker } from '@/components/temp-unit';
import { Button, Card, Pill, Screen, SEVERITY_CLASS, T, Title } from '@/components/ui';
import type * as db from '@/data/db';
import { loadSampleData } from '@/data/sample';
import type { Measurement, MetricKey } from '@/protocol/codec';
import { metricValue, Severity, verdict } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import { feelsLikeLabel, hourDelta, type Issue, issueDetail, issueHeadline, issuesOf, judgeFor, trendSentence } from '@/ui/advice';
import { ago } from '@/ui/format';
import { formatValue, METRICS } from '@/ui/metrics';
import { formatDuration, formatTime, type JudgeMode, judgeMode, sleepPhase } from '@/ui/sleep';
import type { TempUnit } from '@/ui/units';
import { useReading } from '@/ui/use-reading';

export default function RoomScreen() {
  const loaded = useSession((s) => s.loaded);
  const unit = useSession((s) => s.units[0] ?? null);
  if (!loaded) return <Screen>{null}</Screen>;
  if (!unit) return <Welcome />;
  return <Room unit={unit} />;
}

function Welcome() {
  const tempUnit = useSession((s) => s.tempUnit);
  const tempUnitSet = useSession((s) => s.tempUnitSet);
  // The picker starts on the locale's guess; keep it even if the user never touches it.
  const confirmUnit = () => (tempUnitSet ? Promise.resolve() : setTempUnit(tempUnit));
  return (
    <Screen scroll={false}>
      <View className="flex-1 justify-center gap-6 pb-16">
        <AppMark size={88} />
        <View className="gap-3">
          <T className="font-wordmark text-5xl leading-[60px]">Quiesco</T>
          <T className="text-muted text-base leading-6">
            See how your bedroom’s air, temperature, noise and light change through the night. Readings are stored
            on this phone only.
          </T>
        </View>
        <View className="gap-2">
          <T className="font-body-semi text-xs uppercase tracking-widest text-muted px-1">Show temperature in</T>
          <TempUnitPicker value={tempUnit} />
        </View>
        <Button title="Add your Quiesco" onPress={() => confirmUnit().then(() => router.push('/connect'))} />
        {__DEV__ && (
          <Button
            title="Load sample data (development)"
            kind="secondary"
            onPress={() => confirmUnit().then(loadSampleData).then(() => reloadUnits()).then(() => session.set((s) => ({ dataVersion: s.dataVersion + 1 })))}
          />
        )}
      </View>
    </Screen>
  );
}

function Room({ unit }: { unit: db.Unit }) {
  const [now, setNow] = useState(() => Date.now());
  const sleepWindow = useSession((s) => s.sleepWindow);
  const { state, connected, live, reading, measuredAt, recent, chargingV } = useReading(unit);

  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 30_000);
    return () => clearInterval(timer);
  }, []);

  const [refreshing, setRefreshing] = useState(false);
  // Pull to refresh: sync the log when connected, otherwise try to reconnect (which syncs).
  const refresh = () => {
    setRefreshing(true);
    (connected ? syncNow() : reconnect())
      .catch(() => {})
      .finally(() => setRefreshing(false));
  };
  const mode = judgeMode(now, sleepWindow);

  return (
    <Screen onRefresh={refresh} refreshing={refreshing}>
      <Title
        detail={
          <View className="flex-row items-center gap-2">
            {chargingV ? <BatteryIcon volts={chargingV} charging width={26} /> : null}
            <LinkPill state={state} connected={connected} />
          </View>
        }
      >
        Quiesco
      </Title>
      <SleepStrip now={now} />

      {reading ? (
        <>
          <Issues reading={reading} mode={mode} recent={recent} nowS={(measuredAt ?? now) / 1000} />
          <MetricCard metric="co2" reading={reading} mode={mode} />
          <View className="flex-row flex-wrap gap-3">
            {(['temperature', 'humidity', 'light', 'noise'] as MetricKey[]).map((m) => (
              <Tile key={m} metric={m} reading={reading} mode={mode} />
            ))}
          </View>
          <T className="text-xs text-muted px-1">
            {live ? 'Live' : 'Last synced reading'} · measured {ago(measuredAt, now)} · history synced {ago(unit.lastSyncMs, now)}
          </T>
          {state.sync?.state === 'failed' && (
            <T className="text-xs text-bad px-1">{state.sync.message ?? 'Sync stopped.'} Pull down to try again.</T>
          )}
        </>
      ) : (
        <Card>
          <T className="text-muted leading-6">
            {connected ? 'The unit is taking its first measurement. This takes about 11 seconds.' : 'No readings yet. Connect to the unit to see them.'}
          </T>
        </Card>
      )}
    </Screen>
  );
}

function LinkPill({ state, connected }: { state: LinkState; connected: boolean }) {
  if (connected) return <Pill tone="ok">Connected</Pill>;
  if (state.phase === 'connecting' || state.phase === 'pairing') return <Pill tone="off">Connecting…</Pill>;
  return (
    <Pressable onPress={() => reconnect()} accessibilityRole="button" accessibilityLabel="Reconnect">
      <Pill tone="off">Out of range · Retry</Pill>
    </Pressable>
  );
}

/**
 * Tonight's sleep window, and a way into its setting. Its wording changes at
 * the same moment the verdicts do, so the screen says why it started judging.
 */
function SleepStrip({ now }: { now: number }) {
  const t = useTheme();
  const w = useSession((s) => s.sleepWindow);
  const phase = sleepPhase(now, w);
  const range = `${formatTime(phase.fromS)} – ${formatTime(phase.toS)}`;
  let lead: string;
  let detail: string;
  if (!w.set) {
    lead = 'Set your sleep window';
    detail = range;
  } else if (phase.kind === 'during') {
    lead = 'Judging for sleep';
    detail = `until ${formatTime(phase.toS)}`;
  } else if (judgeMode(now, w) === 'sleep') {
    lead = 'Judging for sleep';
    detail = `bed in ${formatDuration(Math.max(5, Math.round(phase.inMin / 5) * 5))}`;
  } else {
    lead = 'Bedtime';
    detail = range;
  }
  return (
    <Pressable
      onPress={() => router.push('/sleep-window')}
      accessibilityRole="button"
      accessibilityLabel={`${lead}, ${detail}. Change sleep window`}
      className="bg-surface rounded-full h-11 px-4 flex-row items-center gap-2.5 active:opacity-70">
      <Svg width={18} height={18} viewBox="0 0 24 24">
        <Path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z" fill="none" stroke={t.primary} strokeWidth={2} strokeLinejoin="round" />
      </Svg>
      <T className="font-body-semi text-sm">{lead}</T>
      <T className="text-sm text-muted grow">{detail}</T>
      <T className="text-muted text-lg">›</T>
    </Pressable>
  );
}

/**
 * Every metric that is off in this mode, stacked worst first, judged by the
 * same references as the Nights tab. By day a comfortable room says nothing.
 */
function Issues({ reading, mode, recent, nowS }: { reading: Measurement; mode: JudgeMode; recent: db.StoredPoint[]; nowS: number }) {
  const t = useTheme();
  const tempUnit = useSession((s) => s.tempUnit);
  const v = verdict(reading);
  if (!v.available) {
    return (
      <Card className="flex-row items-center gap-4">
        <Face size={52} severity={Severity.Warn} paper={t.raised} ink={t.textMuted} />
        <View className="shrink gap-0.5">
          <T className="font-body-semi text-base">Readings unavailable</T>
          <T className="text-muted text-sm leading-5">The sensors didn’t report this cycle. Check the unit in the Unit tab.</T>
        </View>
      </Card>
    );
  }
  const issues = issuesOf(reading, mode);
  if (issues.length === 0) {
    if (mode === 'day') return null;
    return (
      <Card className="flex-row items-center gap-4">
        <Face size={52} severity={Severity.Ok} />
        <View className="shrink gap-0.5">
          <T className={`font-body-semi text-base ${SEVERITY_CLASS[Severity.Ok].text}`}>Comfortable</T>
          <T className="text-sm leading-5 text-muted">Everything is inside its comfort band.</T>
        </View>
      </Card>
    );
  }
  return (
    <>
      {issues.map((issue, i) => {
        const trend = trendSentence(issue.metric, hourDelta(issue.metric, metricValue(reading, issue.metric), recent, nowS), tempUnit);
        const detail = `${trend ? `${trend} ` : ''}${issueDetail(issue, reading, tempUnit, mode)}`;
        return i === 0 ? (
          <LeadIssue key={issue.metric} issue={issue} headline={issueHeadline(issue, mode)} detail={detail} />
        ) : (
          <MoreIssue key={issue.metric} issue={issue} headline={issueHeadline(issue, mode)} detail={detail} />
        );
      })}
    </>
  );
}

/** Inverted (ink ground) so the banner stands apart from the tinted tiles below it. */
function LeadIssue({ issue, headline, detail }: { issue: Issue; headline: string; detail: string }) {
  return (
    <Card className="flex-row items-center gap-4 bg-ink">
      <Face size={52} severity={issue.severity} />
      <View className="shrink gap-0.5">
        <T className="font-body-semi text-base text-bg">{headline}</T>
        <T className="text-sm leading-5 text-bg/80">{detail}</T>
        <Pressable onPress={() => openEvidence(issue.metric)} accessibilityRole="link" hitSlop={10} className="self-start mt-1 active:opacity-60">
          <T className="font-body-semi text-[13px] text-bg underline">Read more</T>
        </Pressable>
      </View>
    </Card>
  );
}

/** A further issue, lighter than the lead so the order of concern is visible. */
function MoreIssue({ issue, headline, detail }: { issue: Issue; headline: string; detail: string }) {
  const t = useTheme();
  return (
    <Card className="flex-row items-start gap-3.5 py-3.5">
      <Face size={36} severity={issue.severity} bare ink={severityColor(t, issue.severity)} />
      <View className="shrink gap-0.5">
        <T className={`font-body-semi text-[15px] ${SEVERITY_CLASS[issue.severity].text}`}>{headline}</T>
        <T className="text-[13px] leading-[18px] text-muted">{detail}</T>
        <Pressable onPress={() => openEvidence(issue.metric)} accessibilityRole="link" hitSlop={10} className="self-start mt-1 active:opacity-60">
          <T className="font-body-semi text-[13px] text-muted underline">Read more</T>
        </Pressable>
      </View>
    </Card>
  );
}

/**
 * The tint is the only visual flag on a tile, so screen readers get the
 * verdict in words instead.
 */
function metricA11y(metric: MetricKey, reading: Measurement, mode: JudgeMode, tempUnit: TempUnit = 'C'): string {
  const s = judgeFor(metric, metricValue(reading, metric), mode);
  const word = s === Severity.Bad ? ', out of range' : s === Severity.Warn ? ', slightly out of range' : '';
  return `${METRICS[metric].label} ${formatValue(metric, metricValue(reading, metric), true, tempUnit)}${word}. Show the last hours`;
}

/** Warn/bad metrics get a tinted card; comfortable and unjudged ones stay plain. */
function flagTint(metric: MetricKey, reading: Measurement, mode: JudgeMode): string {
  const s = judgeFor(metric, metricValue(reading, metric), mode);
  return s === Severity.Warn || s === Severity.Bad ? SEVERITY_CLASS[s].soft : '';
}

function openMetric(metric: MetricKey) {
  router.push({ pathname: '/metric/[metric]', params: { metric } });
}

/** The big CO₂ card. */
function MetricCard({ metric, reading, mode }: { metric: MetricKey; reading: Measurement; mode: JudgeMode }) {
  const tempUnit = useSession((s) => s.tempUnit);
  return (
    <Pressable
      onPress={() => openMetric(metric)}
      accessibilityRole="button"
      accessibilityLabel={metricA11y(metric, reading, mode, tempUnit)}
      className="active:opacity-70">
      <Card className={`gap-1 ${flagTint(metric, reading, mode)}`}>
        <T className="font-body-semi text-xs uppercase tracking-widest text-muted">CO₂ · Air freshness</T>
        <T className="font-number text-[56px] leading-[64px]">
          {formatValue(metric, metricValue(reading, metric), false)}
          <T className="font-body-medium text-lg text-muted"> {METRICS[metric].unit}</T>
        </T>
      </Card>
    </Pressable>
  );
}

function Tile({ metric, reading, mode }: { metric: MetricKey; reading: Measurement; mode: JudgeMode }) {
  const value = metricValue(reading, metric);
  const tempUnit = useSession((s) => s.tempUnit);
  return (
    <Pressable
      onPress={() => openMetric(metric)}
      accessibilityRole="button"
      accessibilityLabel={metricA11y(metric, reading, mode, tempUnit)}
      className="basis-[47%] grow active:opacity-70">
      <Card className={`p-3.5 gap-0.5 ${flagTint(metric, reading, mode)}`}>
        <T className="font-number text-[26px] leading-8">{formatValue(metric, value, true, tempUnit)}</T>
        <T className="text-[13px] text-muted">
          {(metric === 'temperature' && feelsLikeLabel(reading.temperatureC, reading.humidityPct, tempUnit)) || METRICS[metric].label}
        </T>
      </Card>
    </Pressable>
  );
}
