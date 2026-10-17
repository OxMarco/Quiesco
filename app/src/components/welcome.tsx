// Shown in place of the tabs until the phone has a unit: Loading while the
// database answers, then Welcome. Nights and Settings have nothing to show
// without a unit, so their tabs only appear once one is added.

import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { ActivityIndicator, ScrollView, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { reloadUnits, setTempUnit, useSession } from '@/ble/session';
import { AppMark } from '@/components/face';
import { Button, Card, Screen, T, Title } from '@/components/ui';
import { useTheme } from '@/theme';

/**
 * Until the phone's database has answered. Boot loads it; if that has not
 * landed, or failed, this asks again so a bad first read is not a blank screen.
 */
export function Loading() {
  const t = useTheme();
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    let cancelled = false;
    reloadUnits().catch((e) => {
      if (!cancelled) setError(e instanceof Error ? e.message : String(e));
    });
    return () => {
      cancelled = true;
    };
  }, [attempt]);
  return (
    <Screen>
      <Title>Quiesco</Title>
      {error ? (
        <Card className="gap-3">
          <T className="font-body-semi text-base">Couldn’t open your readings</T>
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
      ) : (
        <View className="py-16 items-center">
          <ActivityIndicator color={t.textMuted} accessibilityLabel="Loading" />
        </View>
      )}
    </Screen>
  );
}

export function Welcome() {
  const tempUnit = useSession((s) => s.tempUnit);
  const tempUnitSet = useSession((s) => s.tempUnitSet);
  // The picker starts on the locale's guess; keep it even if the user never touches it.
  const confirmUnit = () => (tempUnitSet ? Promise.resolve() : setTempUnit(tempUnit));
  // No tab bar here (TabsLayout), so the buttons clear the home indicator themselves.
  const insets = useSafeAreaInsets();
  return (
    <Screen scroll={false}>
      {/* Scrolls so large Dynamic Type never pushes the buttons off screen.
          The content sits low, near the thumb. The temperature unit follows the
          locale and is changed in Settings. */}
      <ScrollView className="flex-1" contentContainerClassName="grow justify-end pt-6" contentContainerStyle={{ paddingBottom: insets.bottom + 32 }}>
        <AppMark size={84} />
        <T className="font-wordmark text-5xl leading-[60px] mt-7">Quiesco</T>
        <T className="text-muted text-lg leading-7 mt-2">See how your bedroom changes through the night</T>
        <View className="gap-1.5 mt-11">
          <Button title="Add your Quiesco" onPress={() => confirmUnit().then(() => router.push('/connect'))} />
        </View>
        <T className="text-[13px] text-muted text-center mt-4">Your readings stay on this phone</T>
      </ScrollView>
    </Screen>
  );
}
