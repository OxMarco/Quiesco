// The sleep window: when the user usually sleeps, so a night is judged only
// over those hours. Asked once after adding a unit, changed any time from the
// Room screen or the Unit tab. One window for every night of the week.

import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';

import { setSleepWindow, useSession } from '@/ble/session';
import { Button, Card, T } from '@/components/ui';
import * as db from '@/data/db';
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
  const saved = useSession((s) => s.sleepWindow);
  const unit = useSession((s) => s.units[0] ?? null);
  const [draft, setDraft] = useState<SleepWindow>(saved);
  const [busy, setBusy] = useState(false);
  const lightsOut = useLightsOut(unit?.serial ?? null, saved);

  const valid = validTimes(draft.weekday);
  const suggest =
    lightsOut !== null && circularGap(lightsOut, draft.weekday.bedMin) >= STEP_MIN ? lightsOut : null;

  const save = () => {
    setBusy(true);
    setSleepWindow({ ...draft, weekend: null, set: true })
      .then(() => router.back())
      .finally(() => setBusy(false));
  };

  return (
    <View className="flex-1 bg-bg">
      <ScrollView contentContainerClassName="px-5 pt-6 pb-6 gap-4">
        <View className="flex-row justify-between items-center">
          <T className="font-wordmark text-3xl">Sleep window</T>
          <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
            <T className="text-primary font-body-semi">Cancel</T>
          </Pressable>
        </View>

        <View className="gap-2">
          <T className="font-title text-2xl leading-8">When do you usually sleep?</T>
          <T className="text-muted text-[15px] leading-[22px]">
            Quiesco only judges the room while you’re asleep, so the TV at 22:00 doesn’t count against your night.
          </T>
        </View>

        <TimesEditor times={draft.weekday} onChange={(weekday) => setDraft({ ...draft, weekday })} />

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
                onPress={() => setDraft({ ...draft, weekday: { ...draft.weekday, bedMin: suggest } })}
              />
            </View>
          </Card>
        )}

      </ScrollView>

      <View className="px-5 pb-10 pt-2 gap-2">
        {!valid && (
          <T className="text-bad text-sm text-center">
            A sleep window runs {MIN_SLEEP_MIN / 60} to {MAX_SLEEP_MIN / 60} hours.
          </T>
        )}
        <Button title="Save" busy={busy} disabled={!valid} onPress={save} />
      </View>
    </View>
  );
}

function TimesEditor({ times, onChange }: { times: SleepTimes; onChange: (t: SleepTimes) => void }) {
  return (
    <View className="gap-3">
      <View className="flex-row gap-3">
        <TimeCard label="Bedtime" min={times.bedMin} onChange={(bedMin) => onChange({ ...times, bedMin })} />
        <TimeCard label="Wake up" min={times.wakeMin} onChange={(wakeMin) => onChange({ ...times, wakeMin })} />
      </View>
      <Timeline times={times} />
    </View>
  );
}

function TimeCard({ label, min, onChange }: { label: string; min: number; onChange: (min: number) => void }) {
  return (
    <Card className="flex-1 p-3.5 gap-2">
      <T className="text-[13px] text-muted">{label}</T>
      <T className="font-number text-[32px] leading-10">{formatClock(min)}</T>
      <View className="flex-row gap-2">
        <StepButton label="−" a11y={`${label} earlier`} onPress={() => onChange(wrapMinutes(min - STEP_MIN))} />
        <StepButton label="+" a11y={`${label} later`} onPress={() => onChange(wrapMinutes(min + STEP_MIN))} />
      </View>
    </Card>
  );
}

function StepButton({ label, a11y, onPress }: { label: string; a11y: string; onPress: () => void }) {
  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={a11y}
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
            style={tick.at === 1 ? { right: 0 } : { left: `${tick.at * 100}%`, transform: [{ translateX: tick.at ? -14 : 0 }] }}>
            {tick.label}
          </T>
        ))}
      </View>
      <T className="text-[13px] text-muted">{validTimes(times) ? formatDuration(durationMin(times)) : '–'}</T>
    </View>
  );
}

function circularGap(a: number, b: number): number {
  const d = Math.abs(wrapMinutes(a) - wrapMinutes(b));
  return Math.min(d, 24 * 60 - d);
}

/** When the lights usually go out, from the last two weeks of readings. */
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
