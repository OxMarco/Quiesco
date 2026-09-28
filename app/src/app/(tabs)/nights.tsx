import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { Pressable, ScrollView, View } from 'react-native';

import { useSession } from '@/ble/session';
import { MetricChart } from '@/components/metric-chart';
import { Card, Screen, SectionLabel, T, Title } from '@/components/ui';
import * as db from '@/data/db';
import { Severity } from '@/protocol/comfort';
import { severityColor, useTheme } from '@/theme';
import { formatValue } from '@/ui/metrics';
import { analyzeNight, NIGHT_BANDS, nightLabel, type NightStory, spellReport } from '@/ui/nights';
import { formatTime, viewForNight, windowForNight } from '@/ui/sleep';
import { formatTemp, type TempUnit, toUnit } from '@/ui/units';

type Span = { fromS: number; toS: number };

export default function NightsScreen() {
  const unit = useSession((s) => s.units[0] ?? null);
  const dataVersion = useSession((s) => s.dataVersion);
  const sleepWindow = useSession((s) => s.sleepWindow);
  const tempUnit = useSession((s) => s.tempUnit);
  const [nights, setNights] = useState<string[] | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [points, setPoints] = useState<db.StoredPoint[]>([]);

  useEffect(() => {
    if (!unit) return;
    db.nightsWithData(unit.serial).then((list) => {
      setNights(list);
      setSelected((current) => (current && list.includes(current) ? current : (list[0] ?? null)));
    });
  }, [unit, dataVersion]);

  useEffect(() => {
    if (!unit || !selected) return;
    const { fromS, toS } = viewForNight(selected, sleepWindow);
    db.recordsBetween(unit.serial, fromS, toS).then(setPoints);
  }, [unit, selected, dataVersion, sleepWindow]);

  if (!unit || (nights !== null && nights.length === 0)) {
    return (
      <Screen>
        <Title>Nights</Title>
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
  if (!selected) return <Screen>{null}</Screen>;

  const view = viewForNight(selected, sleepWindow);
  const window = windowForNight(selected, sleepWindow);
  const story = analyzeNight(points, window);

  return (
    <Screen>
      <Title>Nights</Title>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerClassName="gap-2 py-1">
        {(nights ?? []).map((n) => (
          <Pressable
            key={n}
            onPress={() => setSelected(n)}
            accessibilityRole="button"
            accessibilityState={{ selected: n === selected }}
            className={`px-3.5 py-2 rounded-full ${n === selected ? 'bg-primary' : 'bg-surface'}`}>
            <T className={`font-body-medium text-[13px] ${n === selected ? 'text-on-primary' : 'text-ink'}`}>{nightLabel(n)}</T>
          </Pressable>
        ))}
      </ScrollView>

      <WindowRow window={window} />
      {story.samples === 0 && (
        <Card>
          <T className="text-muted leading-6">No readings in your sleeping window.</T>
        </Card>
      )}

      {story.samples > 0 && (
        <>
          <SectionLabel>Background noise</SectionLabel>
          <NoiseCard story={story} points={points} view={view} window={window} />

          <SectionLabel>Air freshness</SectionLabel>
          <Co2Card story={story} points={points} view={view} window={window} />

          <SectionLabel>Room temperature</SectionLabel>
          <TemperatureCard story={story} points={points} view={view} window={window} tempUnit={tempUnit} />

          <SectionLabel>Moisture</SectionLabel>
          <HumidityCard story={story} points={points} view={view} window={window} />

          {story.light && (
            <>
              <SectionLabel>Ambient light</SectionLabel>
              <LightCard story={story} points={points} view={view} window={window} />
            </>
          )}
        </>
      )}
    </Screen>
  );
}

function WindowRow({ window }: { window: Span }) {
  return (
    <View className="flex-row justify-between items-center gap-3 px-1">
      <T className="text-[13px] text-muted shrink">
        Sleeping window from {formatTime(window.fromS)} to {formatTime(window.toS)}
      </T>
      <Pressable onPress={() => router.push('/sleep-window')} accessibilityRole="button" hitSlop={10} className="active:opacity-60">
        <T className="font-body-semi text-[13px] text-primary">Change</T>
      </Pressable>
    </View>
  );
}

interface CardProps {
  story: NightStory;
  points: db.StoredPoint[];
  view: Span;
  window: Span;
}

function CardHead({ label }: { label: string }) {
  return <T className="font-body-semi">{label}</T>;
}

/** The card's report line in its verdict colour (tide, amber or red), with a muted detail under it. */
function Report({ text, severity, detail }: { text: string; severity: Severity; detail: string }) {
  const t = useTheme();
  return (
    <View className="gap-0.5">
      <T className="font-number text-[28px] leading-9" style={{ color: severityColor(t, severity) }}>
        {text}
      </T>
      <T className="text-[13px] text-muted">{detail}</T>
    </View>
  );
}

function othersText(others: number): string {
  if (others === 0) return '';
  return others === 1 ? ' · 1 more spell' : ` · ${others} more spells`;
}

function NoiseCard({ story, points, view, window }: CardProps) {
  const t = useTheme();
  const { average, peak, spells } = story.noise;
  if (!peak) return <Unavailable label="Noise" />;
  const report = spellReport('noise', spells, window);
  const avg = average === null ? '' : `Average ${Math.round(average)} dB · `;
  return (
    <Card className="gap-2.5">
      <CardHead label="Noise" />
      <Report
        text={report.text}
        severity={report.severity}
        detail={`${avg}peak ${formatValue('noise', peak.v)} at ${formatTime(peak.t)}${othersText(report.others)}`}
      />
      <MetricChart
        metric="noise"
        points={points}
        fromS={view.fromS}
        toS={view.toS}
        height={130}
        window={window}
        showBand={false}
        lines={[
          { value: NIGHT_BANDS.noise.okHi, color: t.warn, label: 'noisy' },
          { value: NIGHT_BANDS.noise.warnHi, color: t.bad, label: 'very noisy' },
        ]}
      />
    </Card>
  );
}

function Co2Card({ story, points, view, window }: CardProps) {
  const t = useTheme();
  const { peak, spells } = story.co2;
  if (!peak) return <Unavailable label="CO₂" />;
  const report = spellReport('co2', spells, window);
  return (
    <Card className="gap-2.5">
      <CardHead label="CO₂" />
      <Report
        text={report.text}
        severity={report.severity}
        detail={`Peak ${formatValue('co2', peak.v)} at ${formatTime(peak.t)}${othersText(report.others)}`}
      />
      <MetricChart
        metric="co2"
        points={points}
        fromS={view.fromS}
        toS={view.toS}
        height={130}
        window={window}
        showBand={false}
        lines={[
          { value: NIGHT_BANDS.co2.okHi, color: t.warn, label: 'stuffy' },
          { value: NIGHT_BANDS.co2.warnHi, color: t.bad, label: 'very stuffy' },
        ]}
      />
    </Card>
  );
}

function TemperatureCard({ story, points, view, window, tempUnit }: CardProps & { tempUnit: TempUnit }) {
  const { temperature } = story;
  if (!temperature) return <Unavailable label="Temperature" />;
  const report = spellReport('temperature', temperature.spells, window);
  const feels = temperature.feelsMax;
  const feelsText = feels !== null && feels - temperature.max >= 0.5 ? ` · feels up to ${Math.round(toUnit(feels, tempUnit))}°` : '';
  return (
    <Card className="gap-2.5">
      <CardHead label="Temperature" />
      <Report
        text={report.text}
        severity={report.severity}
        detail={`${formatTemp(temperature.min, tempUnit, false)} – ${formatTemp(temperature.max, tempUnit, false)}°${feelsText}${othersText(report.others)}`}
      />
      <MetricChart
        metric="temperature"
        points={points}
        fromS={view.fromS}
        toS={view.toS}
        height={110}
        window={window}
        band={[NIGHT_BANDS.temperature.okLo, NIGHT_BANDS.temperature.okHi]}
        tempUnit={tempUnit}
      />
    </Card>
  );
}

function HumidityCard({ story, points, view, window }: CardProps) {
  const { humidity } = story;
  if (!humidity) return <Unavailable label="Humidity" />;
  const report = spellReport('humidity', humidity.spells, window);
  return (
    <Card className="gap-2.5">
      <CardHead label="Humidity" />
      <Report
        text={report.text}
        severity={report.severity}
        detail={`${Math.round(humidity.min)} – ${Math.round(humidity.max)} %${othersText(report.others)}`}
      />
      <MetricChart
        metric="humidity"
        points={points}
        fromS={view.fromS}
        toS={view.toS}
        height={110}
        window={window}
        band={[NIGHT_BANDS.humidity.okLo, NIGHT_BANDS.humidity.okHi]}
      />
    </Card>
  );
}

function LightCard({ story, points, view, window }: CardProps) {
  const t = useTheme();
  const { light } = story;
  if (!light) return null;
  const report = spellReport('light', light.spells, window);
  return (
    <Card className="gap-2.5">
      <CardHead label="Light" />
      <Report
        text={report.text}
        severity={report.severity}
        detail={`Brightest ${formatValue('light', light.peak.v)} at ${formatTime(light.peak.t)}${othersText(report.others)}`}
      />
      <MetricChart
        metric="light"
        points={points}
        fromS={view.fromS}
        toS={view.toS}
        height={110}
        window={window}
        lines={[{ value: NIGHT_BANDS.light.warnHi, color: t.bad, label: 'too bright' }]}
      />
    </Card>
  );
}

function Unavailable({ label }: { label: string }) {
  return (
    <Card className="gap-2">
      <CardHead label={label} />
      <T className="text-sm text-muted">No readings during your sleep window.</T>
    </Card>
  );
}
