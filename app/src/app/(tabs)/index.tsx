import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { ActivityIndicator, Alert, Linking, Pressable, ScrollView, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';

import { APP_STORE_URL, FIRMWARE_GUIDE_URL, type LinkState } from '@/ble/link';
import { reconnect, reloadUnits, session, setTempUnit, syncNow, useSession, useShownPrefs, useShownUnit } from '@/ble/session';
import { BatteryIcon } from '@/components/battery';
import { openEvidence } from '@/components/evidence';
import { AppMark, Face } from '@/components/face';
import { SyncCard } from '@/components/sync';
import { UnitTitle } from '@/components/unit-title';
import { Button, Card, Pill, Screen, SEVERITY_CLASS, T, Title } from '@/components/ui';
import type * as db from '@/data/db';
import { isDemo, loadSampleData } from '@/data/sample';
import type { Measurement, MetricKey } from '@/protocol/codec';
import { metricValue, Severity, verdict } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import { feelsLikeLabel, hourDelta, type Issue, issueDetail, issueHeadline, issuesOf, judgeFor, stateWord, trendSentence } from '@/ui/advice';
import { ago } from '@/ui/format';
import { formatValue, METRICS } from '@/ui/metrics';
import { formatDuration, formatTime, type JudgeMode, judgeMode, sleepPhase } from '@/ui/sleep';
import type { TempUnit } from '@/ui/units';
import { useReading } from '@/ui/use-reading';

export default function RoomScreen() {
  const loaded = useSession((s) => s.loaded);
  const unit = useShownUnit();
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
      {/* Scrolls so large Dynamic Type never pushes the buttons off screen.
          The content sits low, near the thumb. The temperature unit follows the
          locale and is changed in Settings. */}
      <ScrollView className="flex-1" contentContainerClassName="grow justify-end pt-6 pb-28">
        <AppMark size={84} />
        <T className="font-wordmark text-5xl leading-[60px] mt-7">Quiesco</T>
        <T className="text-muted text-lg leading-7 mt-2">See how your bedroom changes through the night.</T>
        <View className="gap-1.5 mt-11">
          <Button title="Add your Quiesco" onPress={() => confirmUnit().then(() => router.push('/connect'))} />
          <Button title="Try it with demo data" kind="quiet" busy={demoBusy} onPress={exploreDemo} />
          {demoError && <T className="text-sm text-bad text-center leading-5">{demoError}</T>}
        </View>
        <T className="text-[13px] text-muted text-center mt-4">Your readings stay on this phone.</T>
      </ScrollView>
    </Screen>
  );
}

function Room({ unit }: { unit: db.Unit }) {
  const [now, setNow] = useState(() => Date.now());
  const { sleepWindow } = useShownPrefs();
  const { state, connected, live, reading, measuredAt, recent, chargingV, lowBattery } = useReading(unit);
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
      <UnitTitle
        unit={unit}
        detail={
          <View className="flex-row items-center gap-2">
            {chargingV ? <BatteryIcon volts={chargingV} charging width={26} /> : null}
            {demo ? <DemoPill /> : <LinkPill state={state} connected={connected} />}
          </View>
        }
      >
        Quiesco
      </UnitTitle>
      <SleepStrip now={now} serial={unit.serial} />
      {!demo && state.serial === unit.serial && <VersionCard state={state} />}
      {/* Only this unit's download: the link may be visiting another one. */}
      {!demo && <SyncCard sync={state.serial === unit.serial ? state.sync : null} connected={connected} />}
      {!demo && lowBattery !== null && <BatteryNotice volts={lowBattery} />}

      {reading ? (
        <>
          <Issues reading={reading} mode={mode} recent={recent} nowS={(measuredAt ?? now) / 1000} />
          <Hero metric="co2" reading={reading} mode={mode} />
          <View className="flex-row flex-wrap gap-3.5">
            {(['temperature', 'humidity', 'light', 'noise'] as MetricKey[]).map((m) => (
              <Tile key={m} metric={m} reading={reading} mode={mode} />
            ))}
          </View>
          {reading.pressurePa !== null && <PressureRow pa={reading.pressurePa} />}
          <T className="text-[13px] text-muted text-center mt-2">
            {live || demo ? `Updated ${ago(measuredAt, now)}` : `Last reading ${ago(measuredAt, now)}`}
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
      <Button
        title={old ? 'Open the firmware guide' : 'Update the app'}
        kind="secondary"
        onPress={() => void Linking.openURL(old ? FIRMWARE_GUIDE_URL : APP_STORE_URL).catch(() => {})}
      />
    </Card>
  );
}

/** The unit's battery is nearly empty: the same moment the panel switches to its battery screen. */
function BatteryNotice({ volts }: { volts: number }) {
  const t = useTheme();
  return (
    <Card className="flex-row items-center gap-4" accessibilityLiveRegion="polite">
      <BatteryIcon volts={volts} charging={false} width={34} />
      <View className="shrink grow gap-0.5">
        <T className="font-body-semi text-base" style={{ color: t.bad }}>
          Battery low
        </T>
        <T className="text-sm leading-5 text-muted">Plug the unit into USB-C to charge it.</T>
      </View>
    </Card>
  );
}

/**
 * Air pressure, for interest: it is not judged, so it gets a quiet row rather
 * than a tile, and no chart. Inches of mercury alongside °F, hPa otherwise.
 */
function PressureRow({ pa }: { pa: number }) {
  const tempUnit = useSession((s) => s.tempUnit);
  const value = tempUnit === 'F' ? (pa / 3386.389).toFixed(2) : `${Math.round(pa / 100)}`;
  const unit = tempUnit === 'F' ? 'inHg' : 'hPa';
  return (
    <Card className="flex-row items-baseline justify-between py-3.5">
      <T className="text-sm text-muted">Air pressure</T>
      <T className="font-number text-xl">
        {value}
        <T className="font-body-medium text-sm text-muted"> {unit}</T>
      </T>
    </Card>
  );
}

/** The demo's only label on the Room tab; a tap says what it means. */
function DemoPill() {
  return (
    <Pressable
      onPress={() =>
        Alert.alert('Demo data', 'These readings are made up. Add your Quiesco in Settings, and remove the demo there when you’re done.', [{ text: 'OK' }])
      }
      accessibilityRole="button"
      accessibilityLabel="Demo data. What this means"
      hitSlop={8}
      className="active:opacity-60">
      <Pill tone="off">Demo</Pill>
    </Pressable>
  );
}

function LinkPill({ state, connected }: { state: LinkState; connected: boolean }) {
  // The phone is visiting another unit for a quiet sync; it comes back by itself.
  const background = useSession((s) => (s.background ? (s.units.find((u) => u.serial === s.background)?.name ?? 'a unit') : null));
  if (connected) return <Pill tone="ok">Connected</Pill>;
  if (background) return <Pill tone="off">Syncing {background}…</Pill>;
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
function SleepStrip({ now, serial }: { now: number; serial: string }) {
  const t = useTheme();
  const w = useShownPrefs().sleepWindow;
  const phase = sleepPhase(now, w);
  const range = `${formatTime(phase.fromS)} – ${formatTime(phase.toS)}`;
  let text: string;
  if (!w.set) text = 'Set your sleep window';
  else if (phase.kind === 'during') text = `Sleep mode until ${formatTime(phase.toS)}`;
  else if (judgeMode(now, w) === 'sleep') text = `Bed in ${formatDuration(Math.max(5, Math.round(phase.inMin / 5) * 5))}`;
  else text = `Bed ${range}`;
  return (
    <Pressable
      onPress={() => router.push({ pathname: '/sleep-window', params: { serial } })}
      accessibilityRole="button"
      accessibilityLabel={`${text}. Change sleep window`}
      hitSlop={8}
      className="self-start flex-row items-center gap-2 py-1 active:opacity-60">
      <Svg width={16} height={16} viewBox="0 0 24 24">
        <Path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z" fill="none" stroke={t.primary} strokeWidth={2} strokeLinejoin="round" />
      </Svg>
      <T className={`text-sm ${w.set ? 'text-muted' : 'font-body-semi text-primary'}`}>{text} ›</T>
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

/** The worst issue: a face, what is wrong and what to do. Tapping it opens the evidence. */
function LeadIssue({ issue, headline, detail }: { issue: Issue; headline: string; detail: string }) {
  return (
    <Pressable onPress={() => openEvidence(issue.metric)} accessibilityRole="button" accessibilityHint="Shows why" className="active:opacity-70">
      <Card className="flex-row items-center gap-4">
        <Face size={52} severity={issue.severity} />
        <View className="shrink grow gap-0.5">
          <T className="font-body-semi text-base">{headline}</T>
          <T className="text-sm leading-5 text-muted">{detail}</T>
        </View>
        <T className="text-muted text-lg">›</T>
      </Card>
    </Pressable>
  );
}

/** A further issue, lighter than the lead so the order of concern is visible. */
function MoreIssue({ issue, headline, detail }: { issue: Issue; headline: string; detail: string }) {
  const t = useTheme();
  return (
    <Pressable onPress={() => openEvidence(issue.metric)} accessibilityRole="button" accessibilityHint="Shows why" className="active:opacity-70">
      <Card className="flex-row items-center gap-3.5 py-3.5">
        <Face size={36} severity={issue.severity} bare ink={severityColor(t, issue.severity)} />
        <View className="shrink grow gap-0.5">
          <T className={`font-body-semi text-[15px] ${SEVERITY_CLASS[issue.severity].text}`}>{headline}</T>
          <T className="text-[13px] leading-[18px] text-muted">{detail}</T>
        </View>
        <T className="text-muted text-lg">›</T>
      </Card>
    </Pressable>
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

/** The tile's unit, short: the A-weighting is explained on the Why sheet. */
function shortUnit(metric: MetricKey, tempUnit: TempUnit): string {
  if (metric === 'temperature') return tempUnit === 'F' ? '°F' : '°C';
  if (metric === 'noise') return 'dB';
  return METRICS[metric].unit;
}

/** CO₂, large and on the page itself, with its verdict in one word. */
function Hero({ metric, reading, mode }: { metric: MetricKey; reading: Measurement; mode: JudgeMode }) {
  const t = useTheme();
  const tempUnit = useSession((s) => s.tempUnit);
  const value = metricValue(reading, metric);
  const severity = judgeFor(metric, value, mode);
  const word = stateWord(metric, value, severity, mode);
  return (
    <Pressable
      onPress={() => openMetric(metric)}
      accessibilityRole="button"
      accessibilityLabel={metricA11y(metric, reading, mode, tempUnit)}
      className="items-center pt-8 pb-7 active:opacity-70">
      <T className="font-number text-[88px] leading-[96px]">
        {formatValue(metric, value, false)}
        <T className="font-body-medium text-xl text-muted"> {METRICS[metric].unit}</T>
      </T>
      {severity !== null && (
        <View className="flex-row items-center gap-2 mt-1">
          <Face size={22} severity={severity} bare ink={severityColor(t, severity)} />
          <T className={`font-body-semi text-base ${SEVERITY_CLASS[severity].text}`}>
            {word === 'fresh' ? 'Fresh air' : word.charAt(0).toUpperCase() + word.slice(1)}
          </T>
        </View>
      )}
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
      className="basis-[46%] grow active:opacity-70">
      <Card className={`gap-0.5 ${flagTint(metric, reading, mode)}`}>
        <T className="font-number text-[30px] leading-9">
          {formatValue(metric, value, false, tempUnit)}
          <T className="font-body-medium text-[15px] text-muted"> {shortUnit(metric, tempUnit)}</T>
        </T>
        <T className="text-sm text-muted">
          {(metric === 'temperature' && feelsLikeLabel(reading.temperatureC, reading.humidityPct, tempUnit)) || METRICS[metric].label}
        </T>
      </Card>
    </Pressable>
  );
}
