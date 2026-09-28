// How firm the app's advice is for a metric, drawn the same way wherever it
// appears: a pill naming the kind of source, and three dots for the sheet.

import { router } from 'expo-router';
import { Pressable, View } from 'react-native';

import { EVIDENCE, type Firmness, type JudgedMetric } from '@/ui/advice';

import { T } from './ui';

export function openEvidence(metric: JudgedMetric) {
  router.push({ pathname: '/why/[metric]', params: { metric } });
}

/** Tappable pill with the kind of source, opening the metric's "why" sheet. */
export function EvidenceTag({ metric }: { metric: JudgedMetric }) {
  const e = EVIDENCE[metric];
  const firm = e.firmness >= 2;
  return (
    <Pressable
      onPress={() => openEvidence(metric)}
      accessibilityRole="button"
      accessibilityLabel={`${e.badge}. Why this range`}
      hitSlop={8}
      className={`rounded-full px-2 py-0.5 border active:opacity-60 ${firm ? 'border-primary' : 'border-line'}`}>
      <T className={`font-body-semi text-[11px] ${firm ? 'text-primary' : 'text-muted'}`}>{e.badge.split(' · ')[0]}</T>
    </Pressable>
  );
}

export function FirmnessDots({ firmness }: { firmness: Firmness }) {
  return (
    <View className="flex-row gap-[3px]" accessibilityLabel={`${firmness} of 3`}>
      {[1, 2, 3].map((i) => (
        <View key={i} className={`w-2 h-2 rounded-full ${i <= firmness ? 'bg-primary' : 'bg-line'}`} />
      ))}
    </View>
  );
}
