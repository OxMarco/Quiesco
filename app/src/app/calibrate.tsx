import { router } from 'expo-router';
import { useState } from 'react';
import { ActivityIndicator, Pressable, View } from 'react-native';

import * as linkApi from '@/ble/link';
import { link } from '@/ble/link';
import { useStore } from '@/ble/store';
import { Stepper } from '@/components/controls';
import { Button, Card, T } from '@/components/ui';
import { FrcState } from '@/protocol/codec';
import { useTheme } from '@/theme';

// CO2 forced recalibration (PROTOCOL.md §8).
export default function CalibrateScreen() {
  const t = useTheme();
  const frc = useStore(link, (s) => s.status?.frcState ?? FrcState.Idle);
  const correction = useStore(link, (s) => s.status?.frcCorrectionPpm ?? 0);
  const connected = useStore(link, (s) => s.phase === 'ready');
  const [reference, setReference] = useState(420);
  const [started, setStarted] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const start = () => {
    setError(null);
    linkApi
      .startFrc(reference)
      .then(() => setStarted(true))
      .catch((e) => setError(linkApi.describeError(e)));
  };

  // Idle only means "not picked up yet" while the link is up; once it drops
  // the status is stale, so stop spinning and say so.
  const running =
    started && connected && (frc === FrcState.Pending || frc === FrcState.Soaking || frc === FrcState.Idle);
  const lost = started && !connected && frc !== FrcState.Done && frc !== FrcState.Failed;

  return (
    <View className="flex-1 bg-bg px-5 pt-6 pb-10 gap-5">
      <View className="flex-row justify-between items-center">
        <T className="font-wordmark text-3xl">Recalibrate CO₂</T>
        <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">{started ? 'Done' : 'Cancel'}</T>
        </Pressable>
      </View>

      {!started ? (
        <>
          <Card className="gap-3">
            <Step n={1} text="Put the unit outdoors, or on the sill of a wide-open window, out of direct sun." />
            <Step n={2} text="Wait 5 minutes so it breathes only outdoor air. Keep away from people and exhaust: breath raises CO₂." />
            <Step n={3} text="Start. The unit measures once a minute for 5 minutes, then corrects itself. You can close the app." />
          </Card>
          <Card>
            <Stepper label="Outdoor CO₂" unit="ppm" step={5} min={400} max={2000} value={reference} onChange={setReference} />
            <T className="text-xs text-muted">Fresh outdoor air is about 420 ppm. Change it only if you have a reference meter.</T>
          </Card>
          {error && <T className="text-bad text-sm">{error}</T>}
          <Button title="Start recalibration" disabled={!connected} onPress={start} />
          {!connected && <T className="text-muted text-sm">Connect to the unit first.</T>}
        </>
      ) : (
        <Card className="items-center gap-3 py-8">
          {running && <ActivityIndicator color={t.textMuted} />}
          <T className="font-body-semi text-lg text-center">
            {frc === FrcState.Done
              ? 'Recalibrated'
              : frc === FrcState.Failed
                ? 'Recalibration failed'
                : lost
                  ? 'Disconnected'
                  : frc === FrcState.Soaking
                  ? 'Measuring outdoor air'
                  : 'Waiting for the unit'}
          </T>
          <T className="text-muted text-center leading-6">
            {frc === FrcState.Done
              ? `The sensor corrected itself by ${correction > 0 ? '+' : ''}${correction} ppm. Bring the unit back inside.`
              : frc === FrcState.Failed
                ? 'The CO₂ sensor refused or did not respond. Check the unit in the Settings tab and try again.'
                : lost
                  ? 'The phone lost the unit. A started run carries on by itself; reconnect later and the result shows in Settings.'
                  : frc === FrcState.Soaking
                  ? 'About 5 minutes. Leave the unit where it is.'
                  : 'The run starts with the next measurement.'}
          </T>
        </Card>
      )}
    </View>
  );
}

function Step({ n, text }: { n: number; text: string }) {
  return (
    <View className="flex-row gap-3">
      <View className="w-6 h-6 rounded-full bg-primary/15 items-center justify-center mt-0.5">
        <T className="font-number text-[13px] text-primary">{n}</T>
      </View>
      <T className="flex-1 leading-6">{text}</T>
    </View>
  );
}
