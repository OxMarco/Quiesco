import { NativeTabs } from 'expo-router/unstable-native-tabs';

import { useTheme } from '@/theme';

export default function TabsLayout() {
  const t = useTheme();
  return (
    <NativeTabs tintColor={t.primary} backgroundColor={t.bg} labelStyle={{ color: t.textMuted }}>
      <NativeTabs.Trigger name="index">
        <NativeTabs.Trigger.Label>Room</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf={{ default: 'bed.double', selected: 'bed.double.fill' }} md="bed" />
      </NativeTabs.Trigger>
      <NativeTabs.Trigger name="nights">
        <NativeTabs.Trigger.Label>Nights</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf={{ default: 'moon.stars', selected: 'moon.stars.fill' }} md="bedtime" />
      </NativeTabs.Trigger>
      <NativeTabs.Trigger name="unit">
        <NativeTabs.Trigger.Label>Settings</NativeTabs.Trigger.Label>
        <NativeTabs.Trigger.Icon sf={{ default: 'gearshape', selected: 'gearshape.fill' }} md="settings" />
      </NativeTabs.Trigger>
    </NativeTabs>
  );
}
