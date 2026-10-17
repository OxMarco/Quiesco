import { NativeTabs } from 'expo-router/unstable-native-tabs';

import { useSession } from '@/ble/session';
import { Loading, Welcome } from '@/components/welcome';
import { useTheme } from '@/theme';

export default function TabsLayout() {
  const t = useTheme();
  const loaded = useSession((s) => s.loaded);
  const hasUnit = useSession((s) => s.units.length > 0);
  // No tabs until a unit is added: Nights and Settings would be empty.
  if (!loaded) return <Loading />;
  if (!hasUnit) return <Welcome />;
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
