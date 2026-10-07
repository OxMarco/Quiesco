// "Read more": where a metric's range comes from, and how firm the advice is.

import { router, useLocalSearchParams } from 'expo-router';

import { useSession } from '@/ble/session';
import { Pressable, ScrollView, View } from 'react-native';

import { FirmnessDots } from '@/components/evidence';
import { NotFound, T } from '@/components/ui';
import { EVIDENCE, evidenceScale, FIRMNESS_ORDER, type JudgedMetric, scaleParts, type Zone } from '@/ui/advice';
import { REFERENCE_LABEL } from '@/ui/references';

const ZONE_CLASS: Record<Zone['tone'], string> = {
  ok: 'bg-primary',
  warn: 'bg-warn/60',
  bad: 'bg-bad/80',
};

export default function WhyScreen() {
  const { metric } = useLocalSearchParams<{ metric: string }>();
  const tempUnit = useSession((s) => s.tempUnit);
  const e = EVIDENCE[metric as JudgedMetric];
  if (!e) return <NotFound />;
  return (
    <ScrollView className="flex-1 bg-surface" contentContainerClassName="px-5 pt-6 pb-12 gap-4">
      <View className="flex-row justify-between items-center gap-4">
        <T className="font-title text-2xl shrink">{e.title}</T>
        <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">Close</T>
        </Pressable>
      </View>

      <View className="flex-row items-center gap-2">
        <View className={`rounded-full px-2.5 py-1 ${e.firmness >= 2 ? 'bg-primary' : 'bg-raised'}`}>
          <T className={`font-body-semi text-xs ${e.firmness >= 2 ? 'text-on-primary' : 'text-ink'}`}>{e.badge}</T>
        </View>
        <T className="text-[12px] text-muted">{REFERENCE_LABEL[e.kind]}</T>
      </View>

      <T className="text-[15px] leading-[22px]">{e.body}</T>

      <View className="bg-bg rounded-tile p-4 gap-3">
        <Scale {...evidenceScale(metric as JudgedMetric, tempUnit)} />
        <T className="text-[13px] leading-[19px] text-muted">{e.reference}</T>
      </View>

      <View className="gap-1">
        <T className="font-body-semi text-xs uppercase tracking-widest text-muted">What Quiesco counts at night</T>
        <T className="text-sm leading-[21px]">{e.counts}</T>
      </View>

      <View className="gap-1">
        <T className="font-body-semi text-xs uppercase tracking-widest text-muted mb-1">How firm our advice is</T>
        {FIRMNESS_ORDER.map((f, i) => (
          <View
            key={f.metric}
            className={`flex-row items-center gap-3 py-2 ${i < FIRMNESS_ORDER.length - 1 ? 'border-b border-line' : ''}`}>
            <FirmnessDots firmness={f.firmness} />
            <T className={`text-sm w-[100px] ${f.metric === metric ? 'font-body-semi' : ''}`}>{f.label}</T>
            <T className="text-[13px] text-muted shrink">{f.kind}</T>
          </View>
        ))}
      </View>

      <T className="text-xs leading-[18px] text-muted">Sources: {e.sources}</T>
      <T className="text-xs leading-[18px] text-muted">
        Quiesco is not a medical device. Its readings and advice are for general comfort information only.
      </T>
    </ScrollView>
  );
}

/** The metric's zones on one bar, labelled above and ticked below. */
function Scale({ scale, unit }: ReturnType<typeof evidenceScale>) {
  const { min, max, ticks } = scale;
  const span = max - min;
  const parts = scaleParts(scale);
  return (
    <View className="gap-1.5">
      <View className="flex-row">
        {parts.map((p) => (
          <View key={p.label} style={{ flex: p.share }} className="items-center">
            {p.share >= 0.12 && (
              <T className="font-body-semi text-[11px]" numberOfLines={1}>
                {p.label}
              </T>
            )}
          </View>
        ))}
      </View>
      <View className="flex-row h-3 rounded-full overflow-hidden gap-0.5">
        {parts.map((p) => (
          <View key={p.label} style={{ flex: p.share }} className={ZONE_CLASS[p.tone]} />
        ))}
      </View>
      <View className="h-4">
        {ticks.map((v) => {
          const at = (v - min) / span;
          return (
            <T
              key={`${v}`}
              className="text-[11px] text-muted absolute"
              style={at >= 0.99 ? { right: 0 } : { left: `${at * 100}%`, transform: [{ translateX: at <= 0.01 ? 0 : -10 }] }}>
              {at >= 0.99 ? `${Math.round(v)} ${unit}` : Math.round(v)}
            </T>
          );
        })}
      </View>
    </View>
  );
}
