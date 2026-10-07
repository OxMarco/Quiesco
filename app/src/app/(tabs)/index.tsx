import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { ActivityIndicator, Linking, Pressable, ScrollView, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';

import { FIRMWARE_GUIDE_URL, type LinkState } from '@/ble/link';
import { reconnect, reloadUnits, session, setTempUnit, syncNow, useSession } from '@/ble/session';
import { BatteryIcon } from '@/components/battery';
import { openEvidence } from '@/components/evidence';
import { AppMark, Face } from '@/components/face';
import { SyncCard } from '@/components/sync';
import { TempUnitPicker } from '@/components/temp-unit';
import { Button, Card, Pill, Screen, SEVERITY_CLASS, T, Title } from '@/components/ui';
import type * as db from '@/data/db';
import { isDemo, loadSampleData } from '@/data/sample';
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
  if (!loaded) return <Loading />;
  if (!unit) return <Welcome />;
  return <Room unit={unit} />;
}

/**
 * Until the phone's database has answered. Boot loads it; if that has not
 * landed, or failed, this asks again so a bad first read is not a blank screen.
 */
function Loading() {
  const t = useTheme();
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    let cancelled = false;
    reloadUnits().catch((e) => {
      if (!cancelled) setError(e instanceof Error ? e.message : String(e));
    });
    return () => {
      cancelled = true;
    };
  }, [attempt]);
  return (
    <Screen>
      <Title>Quiesco</Title>
      {error ? (
        <Card className="gap-3">
          <T className="font-body-semi text-base">Couldn’t open your readings</T>
          <T className="text-muted text-sm leading-5">{error}</T>
          <Button
            title="Try again"
            kind="secondary"
            onPress={() => {
              setError(null);
              setAttempt((n) => n + 1);
            }}
          />
        </Card>
      ) : (
        <View className="py-16 items-center">
          <ActivityIndicator color={t.textMuted} accessibilityLabel="Loading" />
        </View>
      )}
    </Screen>
  );
}

function Welcome() {
  const tempUnit = useSession((s) => s.tempUnit);
  const tempUnitSet = useSession((s) => s.tempUnitSet);
  // The picker starts on the locale's guess; keep it even if the user never touches it.
  const confirmUnit = () => (tempUnitSet ? Promise.resolve() : setTempUnit(tempUnit));
  const [demoBusy, setDemoBusy] = useState(false);
  const [demoError, setDemoError] = useState<string | null>(null);
  // Demo mode lets anyone (App Review included) look round without a unit.
  const exploreDemo = () => {
    setDemoBusy(true);
    setDemoError(null);
    confirmUnit()
      .then(loadSampleData)
      .then(() => reloadUnits())
      .then(() => session.set((s) => ({ dataVersion: s.dataVersion + 1 })))
      .catch((e) => setDemoError(`Couldn’t load the demo: ${e instanceof Error ? e.message : String(e)}`))
      .finally(() => setDemoBusy(false));
  };
  return (
    <Screen scroll={false}>
      {/* Scrolls so large Dynamic Type never pushes the buttons off screen. */}
      <ScrollView className="flex-1" contentContainerClassName="grow justify-center gap-6 pt-6 pb-16">
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
        <View className="gap-2">
          <Button title="Explore with demo data" kind="secondary" busy={demoBusy} onPress={exploreDemo} />
          <T className="text-xs text-muted text-center leading-5 px-2">
            No unit yet? Look round with three made-up nights. Remove them any time in Settings.
          </T>
          {demoError && <T className="text-sm text-bad text-center leading-5">{demoError}</T>}
        </View>
      </ScrollView>
    </Screen>
  );
}

function Room({ unit }: { unit: db.Unit }) {
  const [now, setNow] = useState(() => Date.now());
  const sleepWindow = useSession((s) => s.sleepWindow);
  const { state, connected, live, reading, measuredAt, recent, chargingV } = useReading(unit);
  const demo = isDemo(unit);

  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 30_000);
    return () => clearInterval(timer);
  }, []);

  const [refreshing, setRefreshing] = useState(false);
  // Pull to refresh: sync the log when connected, otherwise try to reconnect (which syncs).
  // The demo unit has no radio behind it: refresh only rereads the phone.
  const refresh = () => {
    setRefreshing(true);
    (demo ? reloadUnits().then(() => session.set((s) => ({ dataVersion: s.dataVersion + 1 }))) : connected ? syncNow() : reconnect())
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
            {demo ? <Pill tone="off">Demo</Pill> : <LinkPill state={state} connected={connected} />}
          </View>
        }
      >
        Quiesco
      </Title>
      <SleepStrip now={now} />
      {!demo && <VersionCard state={state} />}
      {!demo && <SyncCard sync={state.sync} connected={connected} lastSyncMs={unit.lastSyncMs} now={now} />}
      {demo && (
        <Card className="gap-1">
          <T className="font-body-semi text-sm">You’re looking at demo data</T>
          <T className="text-[13px] text-muted leading-[18px]">
            These readings are made up. To add a real unit, remove the demo in Settings first.
          </T>
        </Card>
      )}

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
            {demo
              ? `Demo data · measured ${ago(measuredAt, now)}`
              : `${live ? 'Live' : 'Last synced reading'} · measured ${ago(measuredAt, now)} · history synced ${ago(unit.lastSyncMs, now)}`}
          </T>
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

/** Why the unit won't connect when the app and its firmware don't match. */
function VersionCard({ state }: { state: LinkState }) {
  if (state.phase !== 'firmwareTooOld' && state.phase !== 'appTooOld') return null;
  const old = state.phase === 'firmwareTooOld';
  return (
    <Card className="gap-3">
      <T className="font-body-semi text-sm">{old ? 'Update the unit’s firmware' : 'Update the app'}</T>
      <T className="text-[13px] text-muted leading-[18px]">
        {old
          ? 'This unit’s firmware is too old for this app. Update it over USB with the guide on GitHub, then pull down to reconnect.'
          : 'This unit’s firmware is newer than this app. Update the app from the store, then reopen it.'}
      </T>
      {old && <Button title="Open the firmware guide" kind="secondary" onPress={() => void Linking.openURL(FIRMWARE_GUIDE_URL).catch(() => {})} />}
    </Card>
  );
}

function LinkPill({ state, connected }: { state: LinkState; connected: boolean }) {
  if (connected) return <Pill tone="ok">Connected</Pill>;
  if (state.phase === 'connecting' || state.phase === 'pairing') return <Pill tone="off">Connecting…</Pill>;
  // A version mismatch won't clear on retry: say which side needs updating (VersionCard explains).
  if (state.phase === 'firmwareTooOld') return <Pill tone="warn">Unit needs update</Pill>;
  if (state.phase === 'appTooOld') return <Pill tone="warn">App needs update</Pill>;
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
          <T className="text-muted text-sm leading-5">The sensors didn’t report this cycle. Check the unit in the Settings tab.</T>
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
