// "Read more": where a metric's range comes from, and how firm the advice is.

import { router, useLocalSearchParams } from 'expo-router';
import { useState } from 'react';

import { useSession } from '@/ble/session';
import { Pressable, ScrollView, View } from 'react-native';

import { FirmnessDots } from '@/components/evidence';
import { NotFound, Sheet, T } from '@/components/ui';
import { EVIDENCE, evidenceScale, FIRMNESS_ORDER, type JudgedMetric, scaleParts, type Zone } from '@/ui/advice';

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
    <Sheet tone="surface">
    <ScrollView contentContainerClassName="px-4 pt-6 pb-12 gap-4">
      <View className="flex-row justify-between items-center gap-4">
        <T className="font-title text-2xl shrink">{e.title}</T>
        <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">Close</T>
        </Pressable>
      </View>

      <View className="bg-bg rounded-tile p-4 gap-3">
        <Scale {...evidenceScale(metric as JudgedMetric, tempUnit)} />
        <T className="text-[13px] leading-[19px] text-muted">{e.reference}</T>
      </View>

      <View className="flex-row items-center gap-2">
        <View className={`rounded-full px-2.5 py-1 ${e.firmness >= 2 ? 'bg-primary' : 'bg-raised'}`}>
          <T className={`font-body-semi text-[13px] ${e.firmness >= 2 ? 'text-on-primary' : 'text-ink'}`}>{e.badge}</T>
        </View>
      </View>

      {paragraphs(e.body).map((p) => (
        <T key={p} className="text-base leading-6">
          {p}
        </T>
      ))}
      {metric === 'noise' && (
        <T className="text-base leading-6">
          By day Quiesco compares noise with hearing guidance instead: the EPA’s 70 dB(A) 24-hour average and NIOSH’s 85 dB(A)
          8-hour working limit. Its readings are short samples from an uncalibrated microphone, so take them as a rough guide.
        </T>
      )}

      <View className="gap-1 mt-2">
        <T className="font-body-semi text-base">What Quiesco counts at night</T>
        <T className="text-[15px] leading-[22px] text-muted">{e.counts}</T>
      </View>

      <Fold title="How firm our advice is">
        {FIRMNESS_ORDER.map((f, i) => (
          <View
            key={f.metric}
            className={`flex-row items-center gap-3 py-2.5 ${i < FIRMNESS_ORDER.length - 1 ? 'border-b border-line' : ''}`}>
            <FirmnessDots firmness={f.firmness} />
            <T className={`text-[15px] w-[104px] ${f.metric === metric ? 'font-body-semi' : ''}`}>{f.label}</T>
            <T className="text-[13px] text-muted shrink">{f.kind}</T>
          </View>
        ))}
      </Fold>
      <Fold title="Sources">
        <T className="text-[13px] leading-[19px] text-muted">{e.sources}</T>
      </Fold>
      <T className="text-[13px] leading-[18px] text-muted">
        Quiesco is not a medical device. Its readings and advice are for general comfort information only.
      </T>
    </ScrollView>
    </Sheet>
  );
}

/** Long research text in two short paragraphs, split between sentences. */
function paragraphs(text: string): string[] {
  const sentences = text.replace(/([.!?])\s+(?=[A-Z(])/g, '$1\u0000').split('\u0000');
  if (sentences.length < 4) return [text];
  const half = Math.ceil(sentences.length / 2);
  return [sentences.slice(0, half).join(' '), sentences.slice(half).join(' ')];
}

/** Reference material, folded until asked for. */
function Fold({ title, children }: { title: string; children: React.ReactNode }) {
  const [open, setOpen] = useState(false);
  return (
    <View className="gap-2 mt-2">
      <Pressable
        onPress={() => setOpen((o) => !o)}
        accessibilityRole="button"
        accessibilityState={{ expanded: open }}
        hitSlop={8}
        className="flex-row items-center justify-between active:opacity-60">
        <T className="font-body-semi text-base">{title}</T>
        <T className="text-muted text-lg">{open ? '⌃' : '›'}</T>
      </Pressable>
      {open && <View>{children}</View>}
    </View>
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
