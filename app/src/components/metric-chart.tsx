// One metric over time: a single line over its comfortable band. One chart,
// one axis; light is drawn on a log scale like the panel's ledger gauge. On the
// Nights tab it also greys out what lies outside the sleep window and can mark
// reference levels and individual readings. Tapping a chart opens it full
// screen, where dragging along the line shows the value under the finger.
//
// Drawn with victory-native (Skia): the chart supplies the scales and the line,
// everything else is Skia shapes placed with those scales.

import { NotoSans_400Regular, NotoSans_600SemiBold } from '@expo-google-fonts/noto-sans';
import { Circle, DashPathEffect, Group, Line as SkLine, Rect, RoundedRect, Text as SkText, useFont, vec } from '@shopify/react-native-skia';
import { useState } from 'react';
import { Modal, Pressable, useWindowDimensions, View } from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import { useAnimatedReaction, useDerivedValue } from 'react-native-reanimated';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';
import { CartesianChart, Line, useChartPressState, type CartesianChartRenderArg } from 'victory-native';
import { scheduleOnRN } from 'react-native-worklets';

import type { MetricKey } from '@/protocol/codec';
import type { StoredPoint } from '@/data/db';
import { themeVars, useTheme, withAlpha } from '@/theme';
import { formatValue, METRICS, pointValue } from '@/ui/metrics';
import { fromUnit, type TempUnit, toUnit } from '@/ui/units';

import { T } from './ui';

interface Props {
  metric: MetricKey;
  points: StoredPoint[];
  fromS: number;
  toS: number;
  height?: number;
  /** Compact sparkline: no axes or labels. */
  spark?: boolean;
  /** The sleep window: everything outside it is greyed out. */
  window?: { fromS: number; toS: number };
  /** Dashed reference levels. */
  lines?: { value: number; color: string; label?: string }[];
  /** Dashed vertical marks at moments in time, such as bedtime. */
  marks?: { s: number; label?: string }[];
  /** Dots on single readings; hollow ones are shown but were not counted. */
  markers?: { t: number; v: number; hollow?: boolean }[];
  markerColor?: string;
  /** Shade the comfortable band; off where the app gives no verdict. */
  showBand?: boolean;
  /** The band to shade, when it differs from the metric's default (night bands). */
  band?: [number, number];
  /** Axis labels for temperature; the data stays in °C. */
  tempUnit?: TempUnit;
}

// A gap longer than this breaks the line instead of bridging it.
const GAP_S = 45 * 60;
const LABEL_SIZE = 10;
const PROBE_SIZE = 13;

type Row = { t: number; v: number | null };

export function MetricChart(props: Props) {
  const [open, setOpen] = useState(false);
  const t = useTheme();
  if (props.spark) return <ChartBody {...props} />;
  const label = METRICS[props.metric].label;
  return (
    <>
      <Pressable onPress={() => setOpen(true)} accessibilityRole="button" accessibilityLabel={`Show the ${label} chart full screen`}>
        <View pointerEvents="none">
          <ChartBody {...props} />
        </View>
      </Pressable>
      <Modal visible={open} animationType="slide" presentationStyle="fullScreen" onRequestClose={() => setOpen(false)}>
        {/* A modal is its own native window: it needs its own safe-area
            provider, gesture root and theme variables. */}
        <SafeAreaProvider>
          <GestureHandlerRootView style={[{ flex: 1 }, t.dark ? themeVars.night : themeVars.day]}>
            {open && <FullScreenChart {...props} onClose={() => setOpen(false)} />}
          </GestureHandlerRootView>
        </SafeAreaProvider>
      </Modal>
    </>
  );
}

function FullScreenChart({ onClose, ...props }: Props & { onClose: () => void }) {
  const { height } = useWindowDimensions();
  const meta = METRICS[props.metric];
  const tempUnit = props.tempUnit ?? 'C';
  const unit = props.metric === 'temperature' && tempUnit === 'F' ? '°F' : meta.unit;
  const detail = `${clock(props.fromS, true)} – ${clock(props.toS, true)} · drag along the line to read a value`;
  return (
    <SafeAreaView className="flex-1 bg-bg" edges={['top', 'bottom']}>
      <View className="flex-row justify-between items-start px-5 pt-4 pb-2 gap-4">
        <View className="shrink gap-0.5">
          <T className="font-title text-2xl">
            {meta.label} <T className="font-body text-base text-muted">{unit}</T>
          </T>
          <T className="text-[13px] text-muted">{detail}</T>
        </View>
        <Pressable onPress={onClose} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">Close</T>
        </Pressable>
      </View>
      <View className="flex-1 justify-center px-3">
        <ChartBody {...props} height={Math.round(height * 0.6)} interactive />
      </View>
    </SafeAreaView>
  );
}

/** Value on the plotted scale: light is plotted as log10(lux). */
function plotValue(metric: MetricKey, v: number): number {
  const meta = METRICS[metric];
  const clamped = Math.min(meta.max, Math.max(meta.min, v));
  return meta.log ? Math.log10(clamped) : clamped;
}

function ChartBody({
  metric,
  points,
  fromS,
  toS,
  height = 150,
  spark,
  window,
  lines = [],
  marks = [],
  markers = [],
  markerColor,
  showBand = true,
  band = METRICS[metric].band ?? undefined,
  tempUnit = 'C',
  interactive = false,
}: Props & { interactive?: boolean }) {
  const t = useTheme();
  const font = useFont(NotoSans_400Regular, LABEL_SIZE);
  const probeFont = useFont(NotoSans_600SemiBold, PROBE_SIZE);
  const meta = METRICS[metric];
  const { state: press, isActive } = useChartPressState({ x: 0, y: { v: 0 } });
  // The reading under the finger and where it is drawn.
  const [probe, setProbe] = useState<{ t: number; v: number; x: number; y: number } | null>(null);

  useAnimatedReaction(
    () =>
      press.isActive.value
        ? [press.x.value.value, press.y.v.value.value, press.x.position.value, press.y.v.position.value]
        : null,
    (cur, prev) => {
      if (!interactive || (cur === null && prev === null)) return;
      // Over a gap in the data there is no value: keep showing the last one.
      if (cur && !Number.isFinite(cur[1])) return;
      scheduleOnRN(setProbe, cur ? { t: cur[0], v: meta.log ? 10 ** cur[1] : cur[1], x: cur[2], y: cur[3] } : null);
    },
  );

  // Null rows break the line across gaps and across missing readings.
  const data: Row[] = [];
  let prevT: number | null = null;
  for (const p of points) {
    const v = pointValue(p, metric);
    if (prevT !== null && p.t - prevT > GAP_S) data.push({ t: prevT + 1, v: null });
    data.push({ t: p.t, v: v === null ? null : plotValue(metric, v) });
    prevT = p.t;
  }

  const yDomain: [number, number] = meta.log ? [Math.log10(meta.min), Math.log10(meta.max)] : [meta.min, meta.max];
  // Ticks at round numbers of the shown unit, placed on the plotted scale.
  const yTicks = meta.log
    ? [0, 1, 2, 3, 4]
    : metric === 'temperature'
      ? niceTicks(toUnit(meta.min, tempUnit), toUnit(meta.max, tempUnit)).map((u) => fromUnit(u, tempUnit))
      : niceTicks(meta.min, meta.max);
  const yLabel = (v: number) => {
    if (meta.log) {
      const lux = Math.round(10 ** v);
      return lux >= 1000 ? `${lux / 1000}k` : `${lux}`;
    }
    return `${Math.round(metric === 'temperature' ? toUnit(v, tempUnit) : v)}`;
  };
  const [width, setWidth] = useState(0);
  const hours = (toS - fromS) / 3600;
  const xTicks = hourTicks(fromS, toS, hours <= 8 ? 2 : width < 340 ? 4 : 3);
  const axisFont = spark ? null : font;
  const shade = withAlpha(t.text, t.dark ? 0.1 : 0.06);

  const pressLine = useDerivedValue(() => vec(press.x.position.value, 0));
  const pressLineEnd = useDerivedValue(() => vec(press.x.position.value, height));

  const overlays = ({ xScale, yScale, chartBounds, points: pts }: CartesianChartRenderArg<Row, 'v'>) => {
    const { left, right, top, bottom } = chartBounds;
    const clampX = (s: number) => Math.min(right, Math.max(left, xScale(s)));
    const y = (v: number) => yScale(plotValue(metric, v));
    const lastPt = [...pts.v].reverse().find((p) => typeof p.y === 'number');
    return (
      <>
        {window && xScale(window.fromS) > left && (
          <Rect x={left} y={top} width={clampX(window.fromS) - left} height={bottom - top} color={shade} />
        )}
        {window && xScale(window.toS) < right && (
          <Rect x={clampX(window.toS)} y={top} width={right - clampX(window.toS)} height={bottom - top} color={shade} />
        )}
        {showBand && band && (
          <Rect
            x={left}
            y={y(band[1])}
            width={right - left}
            height={Math.max(0, y(band[0]) - y(band[1]))}
            color={withAlpha(t.primary, t.dark ? 0.12 : 0.1)}
          />
        )}
        {!spark && <SkLine p1={vec(left, bottom)} p2={vec(right, bottom)} color={t.line} strokeWidth={1} />}
        {/* Grey, one colour always: the verdict is in the text, and the reference
            lines and their labels stay readable over it. */}
        <Line points={pts.v} color={t.textMuted} strokeWidth={2} connectMissingData={false} />
        {/* Reference lines and their labels go over the data, or it hides them. */}
        {lines.map((l) => (
          <SkLine key={`l${l.value}`} p1={vec(left, y(l.value))} p2={vec(right, y(l.value))} color={l.color} strokeWidth={1}>
            <DashPathEffect intervals={[4, 4]} />
          </SkLine>
        ))}
        {marks
          .filter((m) => m.s >= fromS && m.s <= toS)
          .map((m) => (
            <SkLine key={`v${m.s}`} p1={vec(xScale(m.s), top)} p2={vec(xScale(m.s), bottom)} color={t.textMuted} strokeWidth={1}>
              <DashPathEffect intervals={[3, 3]} />
            </SkLine>
          ))}
        {axisFont &&
          marks
            .filter((m) => m.label && m.s >= fromS && m.s <= toS)
            .map((m) => (
              <SkText
                key={`vt${m.s}`}
                x={xScale(m.s) - 4 - axisFont.measureText(m.label!).width}
                y={top + 10}
                text={m.label!}
                font={axisFont}
                color={t.textMuted}
              />
            ))}
        {axisFont &&
          lines
            .filter((l) => l.label)
            .map((l) => {
              const w = axisFont.measureText(l.label!).width;
              const baseline = y(l.value) - 5;
              // Left end: the latest reading's dot sits at the right edge.
              const x = left + 4;
              return (
                <Group key={`lt${l.value}`}>
                  <RoundedRect x={x - 3} y={baseline - LABEL_SIZE} width={w + 6} height={LABEL_SIZE + 4} r={3} color={withAlpha(t.surface, 0.92)} />
                  <SkText x={x} y={baseline} text={l.label!} font={axisFont} color={l.color} />
                </Group>
              );
            })}
        {markers.map((m) =>
          m.hollow ? (
            <Group key={`m${m.t}`}>
              <Circle cx={xScale(m.t)} cy={y(m.v)} r={4} color={t.surface} />
              <Circle cx={xScale(m.t)} cy={y(m.v)} r={4} color={t.textMuted} style="stroke" strokeWidth={1.5} />
            </Group>
          ) : (
            <Circle key={`m${m.t}`} cx={xScale(m.t)} cy={y(m.v)} r={4} color={markerColor ?? t.bad} />
          ),
        )}
        {lastPt && markers.length === 0 && !isActive && (
          <>
            <Circle cx={lastPt.x} cy={lastPt.y as number} r={5.5} color={t.surface} />
            <Circle cx={lastPt.x} cy={lastPt.y as number} r={3.5} color={t.text} />
          </>
        )}
        {interactive && isActive && (
          <>
            <SkLine p1={pressLine} p2={pressLineEnd} color={t.textMuted} strokeWidth={1} />
            <Circle cx={press.x.position} cy={press.y.v.position} r={5} color={t.text} />
          </>
        )}
        {interactive && isActive && probe && probeFont && probeLabel(probe, chartBounds, probeFont)}
      </>
    );
  };

  // A pill beside the finger's dot, flipped to the left near the right edge.
  function probeLabel(
    p: { t: number; v: number; x: number; y: number },
    { left, right, top, bottom }: { left: number; right: number; top: number; bottom: number },
    f: NonNullable<typeof probeFont>,
  ) {
    const text = `${formatValue(metric, p.v, true, tempUnit)} · ${clock(p.t)}`;
    const w = f.measureText(text).width + 16;
    const h = PROBE_SIZE + 12;
    const x = p.x + 12 + w <= right ? p.x + 12 : Math.max(left, p.x - 12 - w);
    const y = Math.min(bottom - h, Math.max(top, p.y - h - 10));
    return (
      <Group>
        <RoundedRect x={x} y={y} width={w} height={h} r={8} color={t.text} />
        <SkText x={x + 8} y={y + h / 2 + PROBE_SIZE / 2 - 2} text={text} font={f} color={t.surface} />
      </Group>
    );
  }

  return (
    <View style={{ height }} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
      <CartesianChart
        data={data}
        xKey="t"
        yKeys={['v']}
        domain={{ x: [fromS, toS], y: yDomain }}
        padding={spark ? { left: 0, right: 5, top: 5, bottom: 5 } : { left: 34, right: 10, top: 8, bottom: 2 }}
        chartPressState={interactive ? press : undefined}
        // Read values the moment the finger lands, not after a hold.
        gestureLongPressDelay={0}
        xAxis={{
          font: axisFont,
          tickValues: xTicks,
          lineWidth: 0,
          labelColor: t.textMuted,
          formatXLabel: (s) => clock(Number(s)),
        }}
        // Value labels are drawn below (renderOutside): the axis's own labels
        // drop any tick on the bottom edge, which is where the scale starts.
        yAxis={[{ font: null, tickValues: yTicks, lineWidth: 0 }]}
        renderOutside={({ yScale, chartBounds }) =>
          axisFont &&
          yTicks.map((v) => (
            <SkText
              key={`y${v}`}
              x={chartBounds.left - 6 - axisFont.measureText(yLabel(v)).width}
              y={yScale(v) + 3.5}
              text={yLabel(v)}
              font={axisFont}
              color={t.textMuted}
            />
          ))
        }>
        {overlays}
      </CartesianChart>
    </View>
  );
}


function clock(s: number, withDay = false): string {
  return new Date(s * 1000).toLocaleString([], withDay ? { weekday: 'short', hour: '2-digit', minute: '2-digit' } : { hour: '2-digit', minute: '2-digit' });
}

function niceTicks(min: number, max: number): number[] {
  const span = max - min;
  const step = [5, 10, 20, 25, 50, 100, 200, 250, 400, 500].find((s) => span / s <= 4) ?? span / 4;
  const out: number[] = [];
  for (let v = Math.ceil(min / step) * step; v <= max; v += step) out.push(v);
  return out;
}

function hourTicks(fromS: number, toS: number, everyHours: number): number[] {
  const out: number[] = [];
  const start = new Date(fromS * 1000);
  start.setMinutes(0, 0, 0);
  start.setHours(start.getHours() + 1);
  for (let ms = start.getTime(); ms / 1000 < toS; ms += 3600_000) {
    const h = new Date(ms).getHours();
    if (h % everyHours === 0) out.push(ms / 1000);
  }
  return out;
}
