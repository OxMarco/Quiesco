// The log download, made visible: a turning icon and a bar while records
// arrive, a short "up to date" afterwards, and a way to start one by hand.
// The phone syncs on its own at every connect (session.open); this card only
// shows that and offers the same sync on request.

import { useEffect, useState } from 'react';
import { Pressable, View } from 'react-native';
import Animated, { cancelAnimation, Easing, useAnimatedStyle, useReducedMotion, useSharedValue, withRepeat, withTiming } from 'react-native-reanimated';
import Svg, { Path } from 'react-native-svg';

import { link, type SyncProgress } from '@/ble/link';
import { syncNow } from '@/ble/session';
import { useStore } from '@/ble/store';
import type * as db from '@/data/db';
import { isDemo } from '@/data/sample';
import { useTheme } from '@/theme';
import { ago } from '@/ui/format';

import { Card, T } from './ui';

/** How long "Up to date" stays before the card settles back to its idle line. */
const DONE_MS = 4000;

export function SyncCard({ sync, connected, lastSyncMs, now }: { sync: SyncProgress | null; connected: boolean; lastSyncMs: number | null; now: number }) {
  const active = sync?.state === 'waiting' || sync?.state === 'streaming' || sync?.state === 'saving';
  const justDone = useJustDone(sync);

  if (active) return <Downloading sync={sync} />;
  if (sync?.state === 'failed') {
    return (
      <Card className="flex-row items-center gap-3" accessibilityLiveRegion="polite">
        <SyncIcon spinning={false} tone="bad" />
        <View className="flex-1 gap-0.5">
          <T className="font-body-semi text-sm">Sync stopped</T>
          <T className="text-[13px] text-muted leading-[18px]">
            {sync.message ?? 'The download did not finish.'}
            {sync.received > 0 ? ` ${count(sync.received)} readings were saved.` : ''}
          </T>
        </View>
        {connected && <SyncButton title="Try again" />}
      </Card>
    );
  }
  if (justDone && sync) {
    return (
      <Card className="flex-row items-center gap-3" accessibilityLiveRegion="polite">
        <SyncIcon spinning={false} tone="ok" />
        <T className="flex-1 font-body-semi text-sm">
          Up to date{sync.received > 0 ? ` · ${count(sync.received)} new ${sync.received === 1 ? 'reading' : 'readings'}` : ''}
        </T>
      </Card>
    );
  }
  if (!connected) return null;
  return (
    <Card className="flex-row items-center gap-3 py-3">
      <SyncIcon spinning={false} tone="muted" />
      <T className="flex-1 text-[13px] text-muted">History synced {ago(lastSyncMs, now)}</T>
      <SyncButton title="Sync now" />
    </Card>
  );
}

/** The same card for screens that don't already follow the link. */
export function UnitSyncCard({ unit }: { unit: db.Unit | null }) {
  const sync = useStore(link, (s) => s.sync);
  const connected = useStore(link, (s) => s.phase === 'ready' && s.serial === unit?.serial);
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 30_000);
    return () => clearInterval(timer);
  }, []);
  if (!unit || isDemo(unit)) return null;
  return <SyncCard sync={sync} connected={connected} lastSyncMs={unit.lastSyncMs} now={now} />;
}

function Downloading({ sync }: { sync: SyncProgress }) {
  // `expected` is an estimate (the log may have wrapped), so the count says
  // "of about"; once more than that arrived it is plainly wrong, so drop it.
  const expected = sync.expected && sync.expected >= sync.received ? sync.expected : null;
  const fraction = expected ? sync.received / expected : null;
  const detail =
    sync.state === 'waiting'
      ? 'Asking the unit for its log…'
      : expected
        ? `${count(sync.received)} of about ${count(expected)} readings`
        : `${count(sync.received)} readings`;
  return (
    <Card className="gap-3" accessibilityLiveRegion="polite">
      <View className="flex-row items-center gap-3">
        <SyncIcon spinning tone="primary" />
        <View className="flex-1 gap-0.5">
          <T className="font-body-semi text-sm">Downloading readings</T>
          <T className="text-[13px] text-muted">{detail}</T>
        </View>
      </View>
      <ProgressBar fraction={sync.state === 'waiting' ? null : fraction} />
    </Card>
  );
}

function ProgressBar({ fraction }: { fraction: number | null }) {
  const percent = fraction === null ? null : Math.round(fraction * 100);
  return (
    <View
      className="h-1.5 rounded-full bg-raised overflow-hidden"
      accessible
      accessibilityRole="progressbar"
      accessibilityLabel="Download progress"
      accessibilityValue={percent === null ? undefined : { min: 0, max: 100, now: percent }}
    >
      {percent === null ? <Sweep /> : <View className="h-full rounded-full bg-primary" style={{ width: `${Math.max(percent, 2)}%` }} />}
    </View>
  );
}

/** Indeterminate bar: a short segment sliding across while the size is unknown. */
function Sweep() {
  const t = useTheme();
  const reduce = useReducedMotion();
  const x = useSharedValue(0);
  useEffect(() => {
    if (reduce) return;
    x.value = withRepeat(withTiming(1, { duration: 1200, easing: Easing.inOut(Easing.quad) }), -1, true);
    return () => cancelAnimation(x);
  }, [reduce, x]);
  const style = useAnimatedStyle(() => ({ left: `${x.value * 70}%` }));
  return <Animated.View style={[{ position: 'absolute', height: '100%', width: '30%', borderRadius: 999, backgroundColor: t.primary }, style]} />;
}

type Tone = 'primary' | 'ok' | 'bad' | 'muted';

/** Two arrows chasing round a circle; turns while a sync runs. */
function SyncIcon({ spinning, tone }: { spinning: boolean; tone: Tone }) {
  const t = useTheme();
  const reduce = useReducedMotion();
  const turn = useSharedValue(0);
  useEffect(() => {
    if (!spinning || reduce) return;
    turn.value = 0;
    turn.value = withRepeat(withTiming(1, { duration: 1100, easing: Easing.linear }), -1, false);
    return () => cancelAnimation(turn);
  }, [spinning, reduce, turn]);
  const style = useAnimatedStyle(() => ({ transform: [{ rotate: `${turn.value * 360}deg` }] }));
  const color = { primary: t.primary, ok: t.primary, bad: t.bad, muted: t.textMuted }[tone];
  return (
    <Animated.View style={style} accessibilityElementsHidden importantForAccessibility="no-hide-descendants">
      <Svg width={22} height={22} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth={2} strokeLinecap="round" strokeLinejoin="round">
        {tone === 'ok' ? (
          <Path d="M5 12.5l4.5 4.5L19 7.5" />
        ) : (
          <>
            <Path d="M20 12a8 8 0 0 1-13.7 5.6" />
            <Path d="M4 12a8 8 0 0 1 13.7-5.6" />
            <Path d="M17.7 2.5v4h-4" />
            <Path d="M6.3 21.5v-4h4" />
          </>
        )}
      </Svg>
    </Animated.View>
  );
}

function SyncButton({ title }: { title: string }) {
  return (
    <Pressable
      onPress={() => syncNow().catch(() => {})}
      accessibilityRole="button"
      accessibilityLabel={title === 'Sync now' ? 'Sync the unit’s log now' : 'Try the sync again'}
      className="bg-raised rounded-full px-4 h-9 items-center justify-center active:opacity-80"
    >
      <T className="font-body-semi text-[13px]">{title}</T>
    </Pressable>
  );
}

/** True for a few seconds after a sync finishes. */
function useJustDone(sync: SyncProgress | null): boolean {
  // Each finished sync is a new object; remember the one whose moment has passed.
  const [expired, setExpired] = useState<SyncProgress | null>(null);
  useEffect(() => {
    if (sync?.state !== 'done') return;
    const timer = setTimeout(() => setExpired(sync), DONE_MS);
    return () => clearTimeout(timer);
  }, [sync]);
  return sync?.state === 'done' && expired !== sync;
}

function count(n: number): string {
  return n.toLocaleString();
}
