import { useEffect, useState } from 'react';
import { ActivityIndicator, Pressable, ScrollView, View } from 'react-native';

import { useSession, useShownPrefs, useShownUnit } from '@/ble/session';
import { MetricChart } from '@/components/metric-chart';
import { UnitSyncCard } from '@/components/sync';
import { UnitChip } from '@/components/unit-title';
import { Button, Card, Screen, T, Title } from '@/components/ui';
import * as db from '@/data/db';
import { Severity } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import { formatValue } from '@/ui/metrics';
import { analyzeNight, NIGHT_BANDS, nightLabel, type NightStory, spellReport } from '@/ui/nights';
import { formatTime, viewForNight, windowForNight } from '@/ui/sleep';
import { formatTemp, type TempUnit, toUnit } from '@/ui/units';

type Span = { fromS: number; toS: number };

export default function NightsScreen() {
  const unit = useShownUnit();
  const dataVersion = useSession((s) => s.dataVersion);
  const { sleepWindow } = useShownPrefs();
  const tempUnit = useSession((s) => s.tempUnit);
  const t = useTheme();
  const [nights, setNights] = useState<string[] | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [points, setPoints] = useState<db.StoredPoint[]>([]);
  // A failed read shows a card with a retry instead of a blank tab; bumping
  // `attempt` reruns both reads.
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    if (!unit) return;
    db.nightsWithData(unit.serial)
      .then((list) => {
        setNights(list);
        setSelected((current) => (current && list.includes(current) ? current : (list[0] ?? null)));
      })
      .catch((e) => setError(errorText(e)));
  }, [unit, dataVersion, attempt]);

  useEffect(() => {
    if (!unit || !selected) return;
    const { fromS, toS } = viewForNight(selected, sleepWindow);
    db.recordsBetween(unit.serial, fromS, toS).then(setPoints).catch((e) => setError(errorText(e)));
  }, [unit, selected, dataVersion, sleepWindow, attempt]);

  if (error) {
    return (
      <Screen>
        <Title detail={<UnitChip unit={unit} />}>Nights</Title>
        <Card className="gap-3">
          <T className="font-body-semi text-base">Couldn’t load your nights</T>
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
      </Screen>
    );
  }

  if (!unit || (nights !== null && nights.length === 0)) {
    return (
      <Screen>
        <Title detail={<UnitChip unit={unit} />}>Nights</Title>
        <UnitSyncCard unit={unit} />
        <Card>
          <T className="text-muted leading-6">
            {unit
              ? 'No nights yet. The unit logs a reading every few minutes; sync in the morning to see how the night went.'
              : 'Add your Quiesco to start collecting nights.'}
          </T>
        </Card>
      </Screen>
    );
  }
  if (!selected) return <NightsLoading />;

  const view = viewForNight(selected, sleepWindow);
  const window = windowForNight(selected, sleepWindow);
  const story = analyzeNight(points, window);
  const items = nightItems(story, window, tempUnit);
  // Only what went wrong gets a full card; the rest is one line each.
  const issues = items.filter((i) => i.report?.severity === Severity.Bad);
  const rest = items.filter((i) => i.report?.severity !== Severity.Bad);
  const chart = { points, view, window, tempUnit };

  return (
    <Screen>
      <Title detail={<UnitChip unit={unit} />}>Nights</Title>
      <UnitSyncCard unit={unit} />
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerClassName="gap-2 py-1">
        {(nights ?? []).map((n) => (
          <Pressable
            key={n}
            onPress={() => setSelected(n)}
            accessibilityRole="button"
            accessibilityState={{ selected: n === selected }}
            className={`px-4 py-2 rounded-full ${n === selected ? 'bg-primary' : 'bg-surface'}`}>
            <T className={`font-body-medium text-sm ${n === selected ? 'text-on-primary' : 'text-ink'}`}>{nightLabel(n)}</T>
          </Pressable>
        ))}
      </ScrollView>

      {story.samples === 0 ? (
        <Card>
          <T className="text-muted leading-6">No readings in your sleep window.</T>
        </Card>
      ) : (
        <>
          {issues.map((item) => (
            <Card key={item.metric} className="gap-1">
              <T className="text-sm text-muted">{item.name}</T>
              <T className="font-number text-[28px] leading-9" style={{ color: severityColor(t, Severity.Bad) }}>
                {item.report!.text}
              </T>
              <T className="text-sm text-muted">{item.detail}</T>
              <View className="mt-2">
                <NightChart metric={item.metric} {...chart} />
              </View>
            </Card>
          ))}
          {rest.length > 0 && (
            <Card className="py-1">
              {rest.map((item, i) => (
                <NightRow key={item.metric} item={item} chart={chart} last={i === rest.length - 1} />
              ))}
            </Card>
          )}
        </>
      )}
    </Screen>
  );
}

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

function NightsLoading() {
  const t = useTheme();
  const unit = useShownUnit();
  return (
    <Screen>
      <Title detail={<UnitChip unit={unit} />}>Nights</Title>
      <View className="py-16 items-center">
        <ActivityIndicator color={t.textMuted} accessibilityLabel="Loading nights" />
      </View>
    </Screen>
  );
}

type NightMetric = 'noise' | 'co2' | 'temperature' | 'humidity' | 'light';
type ChartInput = { points: db.StoredPoint[]; view: Span; window: Span; tempUnit: TempUnit };

interface NightItem {
  metric: NightMetric;
  name: string;
  /** Null when the metric had no readings in the window. */
  report: ReturnType<typeof spellReport> | null;
  /** The short value on a row when nothing went badly. */
  summary: string;
  /** Peak, range and further spells, under the verdict. */
  detail: string;
}

function othersText(others: number): string {
  if (others === 0) return '';
  return others === 1 ? ' · 1 more spell' : ` · ${others} more spells`;
}

/** Each metric's verdict and lines for one night, in the order the tab lists them. */
function nightItems(story: NightStory, window: Span, tempUnit: TempUnit): NightItem[] {
  const items: NightItem[] = [];
  const none = (metric: NightMetric, name: string): NightItem => ({ metric, name, report: null, summary: 'No readings', detail: '' });
  const ok = (r: ReturnType<typeof spellReport>, quiet: string) => (r.severity === Severity.Ok ? quiet : r.text);

  const { noise, co2, temperature, humidity, light } = story;
  if (noise.peak) {
    const r = spellReport('noise', noise.spells, window);
    items.push({ metric: 'noise', name: 'Noise', report: r, summary: ok(r, 'Calm'), detail: `Peak ${formatValue('noise', noise.peak.v)} at ${formatTime(noise.peak.t)}${othersText(r.others)}` });
  } else items.push(none('noise', 'Noise'));

  if (co2.peak) {
    const r = spellReport('co2', co2.spells, window);
    items.push({ metric: 'co2', name: 'Air', report: r, summary: ok(r, 'Fresh'), detail: `Peak ${formatValue('co2', co2.peak.v)} at ${formatTime(co2.peak.t)}${othersText(r.others)}` });
  } else items.push(none('co2', 'Air'));

  if (temperature) {
    const r = spellReport('temperature', temperature.spells, window);
    const range = `${formatTemp(temperature.min, tempUnit, false)} – ${formatTemp(temperature.max, tempUnit, false)}°`;
    const feels = temperature.feelsMax;
    const feelsText = feels !== null && feels - temperature.max >= 0.5 ? ` · feels up to ${Math.round(toUnit(feels, tempUnit))}°` : '';
    items.push({ metric: 'temperature', name: 'Temperature', report: r, summary: ok(r, range), detail: `${range}${feelsText}${othersText(r.others)}` });
  } else items.push(none('temperature', 'Temperature'));

  if (humidity) {
    const r = spellReport('humidity', humidity.spells, window);
    const range = `${Math.round(humidity.min)} – ${Math.round(humidity.max)}%`;
    items.push({ metric: 'humidity', name: 'Humidity', report: r, summary: ok(r, range), detail: `${range}${othersText(r.others)}` });
  } else items.push(none('humidity', 'Humidity'));

  if (light) {
    const r = spellReport('light', light.spells, window);
    items.push({ metric: 'light', name: 'Light', report: r, summary: ok(r, 'Dark'), detail: `Brightest ${formatValue('light', light.peak.v)} at ${formatTime(light.peak.t)}${othersText(r.others)}` });
  }
  return items;
}

/** A metric that was fine or only a little off: one line, opening to its chart. */
function NightRow({ item, chart, last }: { item: NightItem; chart: ChartInput; last: boolean }) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  const severity = item.report?.severity ?? null;
  const dot = severity === null ? t.line : severityColor(t, severity);
  const row = (
    <View className="flex-row items-center gap-3 py-3.5">
      <View className="w-2.5 h-2.5 rounded-full" style={{ backgroundColor: dot }} />
      <T className="text-base">{item.name}</T>
      <T
        className={`text-[15px] text-right shrink grow ${severity === Severity.Warn ? 'font-body-medium' : 'text-muted'}`}
        style={severity === Severity.Warn ? { color: severityColor(t, severity) } : undefined}
        numberOfLines={2}>
        {item.summary}
      </T>
      {item.report && <T className="text-muted text-lg">{open ? '⌃' : '›'}</T>}
    </View>
  );
  return (
    <View className={last ? '' : 'border-b border-line'}>
      {item.report ? (
        <Pressable
          onPress={() => setOpen((o) => !o)}
          accessibilityRole="button"
          accessibilityState={{ expanded: open }}
          accessibilityLabel={`${item.name}, ${item.summary}. ${open ? 'Hide' : 'Show'} the chart`}
          className="active:opacity-60">
          {row}
        </Pressable>
      ) : (
        row
      )}
      {open && (
        <View className="gap-2 pb-4">
          <T className="text-sm text-muted">{item.detail}</T>
          <NightChart metric={item.metric} {...chart} />
        </View>
      )}
    </View>
  );
}

/** The night's chart for one metric: no value scale, just the lines that matter, labelled with their numbers. */
function NightChart({ metric, points, view, window, tempUnit }: ChartInput & { metric: NightMetric }) {
  const t = useTheme();
  const b = NIGHT_BANDS[metric];
  const common = { metric, points, fromS: view.fromS, toS: view.toS, window, height: 120, quietAxis: true, tempUnit } as const;
  switch (metric) {
    case 'noise':
    case 'co2':
      return (
        <MetricChart
          {...common}
          showBand={false}
          lines={[
            { value: b.okHi, color: t.warn, label: `${b.okHi}` },
            { value: b.warnHi, color: t.bad, label: `${b.warnHi}` },
          ]}
        />
      );
    case 'temperature':
    case 'humidity':
      return <MetricChart {...common} band={[b.okLo, b.okHi]} />;
    case 'light':
      return <MetricChart {...common} lines={[{ value: b.warnHi, color: t.bad, label: `${b.warnHi} lx` }]} />;
  }
}
