// The sleep window: when the room's sleeper usually sleeps, so a night is
// judged only over those hours. Each unit has its own. Asked after adding a
// unit, changed any time from the Room screen or the unit's settings. One
// window for every night of the week.

import { router, useLocalSearchParams } from 'expo-router';
import { useEffect, useRef, useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';

import { prefsFor, recordedSleep, setSleepWindow, shownUnit, useSession } from '@/ble/session';
import { Button, Card, Sheet, SheetHeader, T } from '@/components/ui';
import * as db from '@/data/db';
import { HEALTH_STORE } from '@/health/sleep';
import { usualTimes } from '@/ui/health-sleep';
import {
  durationMin,
  formatClock,
  formatDuration,
  MAX_SLEEP_MIN,
  MIN_SLEEP_MIN,
  type SleepTimes,
  type SleepWindow,
  STEP_MIN,
  typicalLightsOut,
  validTimes,
  wrapMinutes,
} from '@/ui/sleep';

export default function SleepWindowScreen() {
  // `setup=1` when this follows adding a unit: the last step. `serial` names
  // the unit; without it, the one on screen.
  const { setup, serial } = useLocalSearchParams<{ setup?: string; serial?: string }>();
  const unit = useSession((s) => s.units.find((u) => u.serial === serial) ?? shownUnit(s));
  const saved = useSession((s) => prefsFor(s, unit?.serial).sleepWindow);
  const several = useSession((s) => s.units.length > 1);
  const [draft, setDraft] = useState<SleepWindow>(saved);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const lightsOut = useLightsOut(unit?.serial ?? null, saved);
  const usual = useUsualSleep();

  const valid = validTimes(draft.times);
  // Recorded sleep beats lights out: it has the wake time too.
  const fromHealth =
    usual !== null && (usual.bedMin !== draft.times.bedMin || usual.wakeMin !== draft.times.wakeMin) ? usual : null;
  const suggest =
    usual === null && lightsOut !== null && circularGap(lightsOut, draft.times.bedMin) >= STEP_MIN ? lightsOut : null;

  const save = () => {
    if (!unit) return;
    setBusy(true);
    setError(null);
    setSleepWindow(unit.serial, { ...draft, set: true })
      .then(() => router.back())
      .catch((e) => setError(`Couldn’t save the sleep window: ${e instanceof Error ? e.message : String(e)}`))
      .finally(() => setBusy(false));
  };

  return (
    <Sheet>
      <ScrollView contentContainerClassName="px-4 pt-6 pb-6 gap-4">
        <SheetHeader
          eyebrow={setup === '1' ? 'Step 3 of 3' : several && unit ? unit.name : undefined}
          title="When do you usually sleep?"
          action="Cancel"
          onAction={() => router.back()}
        />
        <T className="text-muted text-[15px] leading-[22px]">Quiesco judges the room only while you sleep</T>

        <TimesEditor times={draft.times} onChange={(update) => setDraft((d) => ({ ...d, times: update(d.times) }))} />

        {fromHealth !== null && (
          <Card className="bg-primary/10 gap-2">
            <T className="text-sm leading-5">
              {`${HEALTH_STORE} has you asleep from about `}
              <T className="font-body-semi text-sm">{`${formatClock(fromHealth.bedMin)} to ${formatClock(fromHealth.wakeMin)}`}</T>
              {' on most nights'}
            </T>
            <View className="self-start">
              <Button title="Use these times" kind="secondary" onPress={() => setDraft((d) => ({ ...d, times: fromHealth }))} />
            </View>
          </Card>
        )}

        {suggest !== null && (
          <Card className="bg-primary/10 gap-2">
            <T className="text-sm leading-5">
              On most nights your lights go out around <T className="font-body-semi text-sm">{formatClock(suggest)}</T>.
              Start the window then?
            </T>
            <View className="self-start">
              <Button
                title={`Use ${formatClock(suggest)}`}
                kind="secondary"
                onPress={() => setDraft((d) => ({ ...d, times: { ...d.times, bedMin: suggest } }))}
              />
            </View>
          </Card>
        )}

      </ScrollView>

      <View className="px-4 pb-10 pt-2 gap-2">
        {!valid && (
          <T className="text-bad text-sm text-center">
            A sleep window runs {MIN_SLEEP_MIN / 60} to {MAX_SLEEP_MIN / 60} hours
          </T>
        )}
        {error && <T className="text-bad text-sm text-center">{error}</T>}
        <Button title="Save" busy={busy} disabled={!valid || !unit} onPress={save} />
      </View>
    </Sheet>
  );
}

type Update = (t: SleepTimes) => SleepTimes;

// Edits are updater functions so a held button keeps stepping from the latest
// value instead of the one captured when the press began.
function TimesEditor({ times, onChange }: { times: SleepTimes; onChange: (update: Update) => void }) {
  return (
    <View className="gap-3">
      <View className="flex-row gap-3">
        <TimeCard label="Bedtime" min={times.bedMin} onStep={(d) => onChange((t) => ({ ...t, bedMin: wrapMinutes(t.bedMin + d) }))} />
        <TimeCard label="Wake up" min={times.wakeMin} onStep={(d) => onChange((t) => ({ ...t, wakeMin: wrapMinutes(t.wakeMin + d) }))} />
      </View>
      <Timeline times={times} />
    </View>
  );
}

function TimeCard({ label, min, onStep }: { label: string; min: number; onStep: (deltaMin: number) => void }) {
  return (
    <Card className="flex-1 p-3.5 gap-2">
      <T className="text-[13px] text-muted">{label}</T>
      <T className="font-number text-[32px] leading-10" numberOfLines={1} adjustsFontSizeToFit>
        {formatClock(min)}
      </T>
      <View className="flex-row gap-2">
        <StepButton label="−" a11y={`${label} earlier`} onPress={() => onStep(-STEP_MIN)} />
        <StepButton label="+" a11y={`${label} later`} onPress={() => onStep(STEP_MIN)} />
      </View>
    </Card>
  );
}

const REPEAT_DELAY_MS = 350;
const REPEAT_EVERY_MS = 110;

/** One step per tap; hold to keep stepping, so a two-hour move is a moment, not eight taps. */
function StepButton({ label, a11y, onPress }: { label: string; a11y: string; onPress: () => void }) {
  // `onPress` only hands an updater to setState, so the closure from the press
  // that started the hold stays correct for every repeat.
  const timer = useRef<ReturnType<typeof setInterval> | null>(null);
  const stop = () => {
    if (timer.current) clearInterval(timer.current);
    timer.current = null;
  };
  useEffect(() => stop, []);
  return (
    <Pressable
      onPress={onPress}
      onLongPress={() => {
        onPress();
        timer.current = setInterval(onPress, REPEAT_EVERY_MS);
      }}
      delayLongPress={REPEAT_DELAY_MS}
      onPressOut={stop}
      accessibilityRole="button"
      accessibilityLabel={a11y}
      accessibilityHint="Hold to keep changing"
      className="flex-1 h-11 rounded-full bg-raised items-center justify-center active:opacity-60">
      <T className="text-lg leading-6">{label}</T>
    </Pressable>
  );
}

/** The window on an 18:00 – 12:00 night. */
function Timeline({ times }: { times: SleepTimes }) {
  const spanMin = 18 * 60;
  const start = wrapMinutes(times.bedMin - 18 * 60);
  const length = Math.min(durationMin(times), Math.max(0, spanMin - start));
  const ticks = [
    { at: 0, label: formatClock(18 * 60) },
    { at: 6 / 18, label: formatClock(0) },
    { at: 12 / 18, label: formatClock(6 * 60) },
    { at: 1, label: formatClock(12 * 60) },
  ];
  // Labels are measured, not guessed, so "12 AM" centres as well as "00:00".
  const [widths, setWidths] = useState<Record<number, number>>({});
  return (
    <View className="gap-1.5 px-1">
      <View className="h-2.5 rounded-full bg-line overflow-hidden">
        <View
          className="absolute h-full rounded-full bg-primary"
          style={{ left: `${(start / spanMin) * 100}%`, width: `${(length / spanMin) * 100}%` }}
        />
      </View>
      <View className="h-4">
        {ticks.map((tick) => (
          <T
            key={tick.at}
            className="text-[11px] text-muted absolute"
            numberOfLines={1}
            onLayout={(e) => {
              const w = e.nativeEvent.layout.width;
              setWidths((prev) => (prev[tick.at] === w ? prev : { ...prev, [tick.at]: w }));
            }}
            style={
              tick.at === 1
                ? { right: 0 }
                : { left: `${tick.at * 100}%`, transform: [{ translateX: tick.at ? -(widths[tick.at] ?? 0) / 2 : 0 }] }
            }>
            {tick.label}
          </T>
        ))}
      </View>
      <View className="flex-row items-baseline justify-between pt-1">
        <T className="text-[13px] text-muted">Asleep</T>
        <T className="font-body-semi text-[15px]">{validTimes(times) ? formatDuration(durationMin(times)) : '–'}</T>
      </View>
    </View>
  );
}

function circularGap(a: number, b: number): number {
  const d = Math.abs(wrapMinutes(a) - wrapMinutes(b));
  return Math.min(d, 24 * 60 - d);
}

/** When the lights usually go out, from the last two weeks of readings. */
/** The usual sleep times over the last three weeks the health store recorded, while that setting is on. */
function useUsualSleep(): SleepTimes | null {
  const healthSleep = useSession((s) => s.healthSleep);
  const [result, setResult] = useState<SleepTimes | null>(null);
  useEffect(() => {
    if (!healthSleep) return;
    const now = Date.now();
    recordedSleep(now - 21 * 86400_000, now)
      .then((spans) => setResult(usualTimes(spans)))
      .catch(() => {});
  }, [healthSleep]);
  return healthSleep ? result : null;
}

function useLightsOut(serial: string | null, w: SleepWindow): number | null {
  const [result, setResult] = useState<number | null>(null);
  useEffect(() => {
    if (!serial) return;
    const nowS = Date.now() / 1000;
    Promise.all([db.nightsWithData(serial, 14), db.recordsBetween(serial, nowS - 15 * 86400, nowS)]).then(
      ([nights, points]) => setResult(typicalLightsOut(points, nights, w)),
    );
  }, [serial, w]);
  return result;
}
