// The unit switcher: every unit on this phone with how its room is doing, the
// shown one ticked. Picking one shows it on every tab and connects to it.

import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';
import Svg, { Path } from 'react-native-svg';

import { link } from '@/ble/link';
import { prefsFor, selectUnit, shownUnit, useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import { Face } from '@/components/face';
import { Button, Card, Sheet, SheetHeader, T } from '@/components/ui';
import * as db from '@/data/db';
import { Severity } from '@/protocol/comfort';
import { trace } from '@/debug/trace';
import { severityColor, useTheme } from '@/theme';
import { issueHeadline, issuesOf } from '@/ui/advice';
import { ago } from '@/ui/format';
import { judgeMode } from '@/ui/sleep';
import { fromPoint } from '@/ui/use-reading';

export default function SwitchUnitScreen() {
  const units = useSession((s) => s.units);
  const shown = useSession((s) => shownUnit(s)?.serial ?? null);
  const dataVersion = useSession((s) => s.dataVersion);
  const [latest, setLatest] = useState<Record<string, db.LatestPoint | null>>({});
  // Frozen at open, like the metric page: the sheet is short-lived.
  const [now] = useState(() => Date.now());

  useEffect(() => {
    let cancelled = false;
    Promise.all(units.map(async (u) => [u.serial, await db.latestPoint(u.serial)] as const))
      .then((pairs) => !cancelled && setLatest(Object.fromEntries(pairs)))
      .catch((e) => trace('switcher load failed', e));
    return () => {
      cancelled = true;
    };
  }, [units, dataVersion]);

  const pick = (serial: string) => {
    router.back();
    selectUnit(serial).catch((e) => trace('select failed', e));
  };

  return (
    <Sheet>
      <ScrollView contentContainerClassName="px-4 pt-6 pb-10 gap-4">
        <SheetHeader title="Your units" action="Done" onAction={() => router.back()} />
        <Card className="py-1">
          {units.map((u, i) => (
            <UnitRow
              key={u.serial}
              unit={u}
              point={latest[u.serial] ?? null}
              now={now}
              selected={u.serial === shown}
              last={i === units.length - 1}
              onPress={() => pick(u.serial)}
            />
          ))}
        </Card>
        <T className="text-[13px] text-muted leading-[18px] px-1">
          The phone talks to one unit at a time. Units in range sync one after another when the app opens, so their nights
          are ready when you switch.
        </T>
        <Button title="Add another Quiesco" kind="secondary" onPress={() => router.replace('/connect')} />
      </ScrollView>
    </Sheet>
  );
}

function UnitRow({
  unit,
  point,
  now,
  selected,
  last,
  onPress,
}: {
  unit: db.Unit;
  point: db.LatestPoint | null;
  now: number;
  selected: boolean;
  last: boolean;
  onPress: () => void;
}) {
  const t = useTheme();
  const connected = useStore(link, (s) => s.phase === 'ready' && s.serial === unit.serial);
  const syncing = useSession((s) => s.background === unit.serial);
  const window = useSession((s) => prefsFor(s, unit.serial).sleepWindow);
  const issue = point ? (issuesOf(fromPoint(point), judgeMode(now, window))[0] ?? null) : null;
  const status = connected
      ? 'Connected'
      : syncing
        ? 'Syncing…'
        : unit.lastSyncMs
          ? `Synced ${ago(unit.lastSyncMs)}`
          : 'Not synced yet';
  const room = !point ? null : issue ? issueHeadline(issue) : 'All comfortable';
  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityState={{ selected }}
      accessibilityLabel={[unit.name, status, room].filter(Boolean).join('. ')}
      className={`flex-row items-center gap-3 py-3.5 active:opacity-60 ${last ? '' : 'border-b border-line'}`}>
      <Face size={40} severity={point ? (issue?.severity ?? Severity.Ok) : undefined} />
      <View className="flex-1 gap-0.5">
        <T className="font-body-semi text-base" numberOfLines={1}>
          {unit.name}
        </T>
        <T className="text-[13px] text-muted" numberOfLines={1}>
          {room ? `${status} · ${room}` : status}
        </T>
      </View>
      {selected && (
        <Svg width={22} height={22} viewBox="0 0 24 24">
          <Path d="M5 12.5l4.5 4.5L19 7.5" fill="none" stroke={severityColor(t, null)} strokeWidth={2.2} strokeLinecap="round" strokeLinejoin="round" />
        </Svg>
      )}
    </Pressable>
  );
}
