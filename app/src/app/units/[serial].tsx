// One unit's settings, pushed from the Settings tab's list.

import { router, useLocalSearchParams } from 'expo-router';
import { Pressable } from 'react-native';

import { useSession } from '@/ble/session';
import { UnitSettings } from '@/components/unit-settings';
import { NotFound, Screen, T, Title } from '@/components/ui';

export default function UnitScreen() {
  const { serial } = useLocalSearchParams<{ serial: string }>();
  const unit = useSession((s) => s.units.find((u) => u.serial === serial) ?? null);
  // Removed while open (or a stale link): nothing left to show.
  if (!unit) return <NotFound message="This unit is no longer on this phone." />;
  return (
    <Screen>
      <Pressable
        onPress={() => (router.canGoBack() ? router.back() : router.replace('/unit'))}
        accessibilityRole="button"
        accessibilityLabel="Back to Settings"
        hitSlop={12}
        className="self-start pt-3 active:opacity-60">
        <T className="text-primary text-base">‹ Settings</T>
      </Pressable>
      <Title>{unit.name}</Title>
      <UnitSettings unit={unit} />
    </Screen>
  );
}
