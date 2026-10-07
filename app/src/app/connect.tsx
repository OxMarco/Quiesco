import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { ActivityIndicator, Linking, Pressable, View } from 'react-native';

import { describeError, FIRMWARE_GUIDE_URL, link } from '@/ble/link';
import { scanner, startScan, stopScan } from '@/ble/scanner';
import { open, session, useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import * as ble from '@/ble/transport';
import { Face } from '@/components/face';
import { Button, Card, T } from '@/components/ui';
import { Severity } from '@/protocol/comfort';
import { useTheme } from '@/theme';

export default function ConnectScreen() {
  const t = useTheme();
  const phase = useStore(link, (s) => s.phase);
  const error = useStore(link, (s) => s.error);
  const radio = useSession((s) => s.radio);
  const found = useStore(scanner, (s) => s.found);
  const scanning = useStore(scanner, (s) => s.scanning);
  const permission = useStore(scanner, (s) => s.permitted);
  const [chosen, setChosen] = useState<ble.Advert | null>(null);
  // Failures after the link is up (saving the unit, say) never reach the link's own error.
  const [openError, setOpenError] = useState<string | null>(null);

  useEffect(() => {
    if (radio === 'on' || radio === 'unknown') startScan();
    return stopScan;
  }, [radio]);

  const connectTo = (id: string) => {
    setOpenError(null);
    open(id).catch((e) => setOpenError(describeError(e)));
  };

  const choose = (a: ble.Advert) => {
    stopScan();
    setChosen(a);
    connectTo(a.id);
  };

  useEffect(() => {
    if (chosen && phase === 'ready' && !openError) {
      // First unit on this phone: ask for the sleep window before anything else.
      const next = () => (session.get().sleepWindow.set ? router.back() : router.replace('/sleep-window'));
      const timer = setTimeout(next, 900);
      return () => clearTimeout(timer);
    }
  }, [chosen, phase, openError]);

  return (
    <View className="flex-1 bg-bg px-5 pt-6 pb-10 gap-5">
      <View className="flex-row justify-between items-center">
        <T className="font-wordmark text-3xl">Add your Quiesco</T>
        <Pressable onPress={() => router.back()} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">Close</T>
        </Pressable>
      </View>

      {chosen ? (
        <Pairing
          phase={openError ? 'error' : phase}
          error={openError ?? error}
          name={chosen.name}
          onRetry={() => connectTo(chosen.id)}
          onBack={() => { setOpenError(null); setChosen(null); startScan(); }}
        />
      ) : radio === 'unsupported' ? (
        <Card><T className="leading-6">This device has no Bluetooth, so it can’t talk to a Quiesco unit.</T></Card>
      ) : radio === 'off' ? (
        <Card><T className="leading-6">Bluetooth is off. Turn it on in Settings or Control Center to find your unit.</T></Card>
      ) : radio === 'unauthorized' || permission === false ? (
        <View className="gap-3">
          <Card><T className="leading-6">Quiesco needs Bluetooth permission to find your unit. Allow it in Settings, then come back.</T></Card>
          <Button title="Open Settings" onPress={() => void Linking.openSettings().catch(() => {})} />
        </View>
      ) : (
        <>
          <T className="text-muted leading-6">
            Keep the unit nearby and plug it into USB power. During setup, enter the key shown on the unit’s screen.
          </T>
          <View className="gap-2">
            {found.map((a) => (
              <Pressable key={a.id} onPress={() => choose(a)} accessibilityRole="button" className="active:opacity-70">
                <Card className="flex-row items-center gap-4">
                  <Face size={40} />
                  <View className="flex-1">
                    <T className="font-body-semi text-base">{a.name}</T>
                    <T className="text-[13px] text-muted">{signal(a.rssi)}</T>
                  </View>
                  <T className="text-primary font-body-semi">Connect</T>
                </Card>
              </Pressable>
            ))}
          </View>
          {scanning ? (
            <View className="flex-row items-center gap-3 px-1">
              <ActivityIndicator color={t.textMuted} />
              <T className="text-muted">Looking for units…</T>
            </View>
          ) : (
            <View className="gap-3">
              {found.length === 0 && (
                <T className="text-muted leading-6">
                  No units found. If the unit is set to connect only after measuring, plug it into USB or wait for its
                  next measurement.
                </T>
              )}
              <Button title="Search again" kind="secondary" onPress={startScan} />
            </View>
          )}
        </>
      )}
    </View>
  );
}

function signal(rssi: number) {
  if (rssi > -60) return 'Right here';
  if (rssi > -75) return 'Nearby';
  return 'Far away · move closer';
}

function Pairing({
  phase,
  error,
  name,
  onRetry,
  onBack,
}: {
  phase: string;
  error: string | null;
  name: string;
  onRetry: () => void;
  onBack: () => void;
}) {
  const t = useTheme();
  const content: Record<string, { face: Severity | undefined; title: string; body: string }> = {
    connecting: { face: undefined, title: `Connecting to ${name}`, body: 'This takes a few seconds.' },
    pairing: {
      face: undefined,
      title: 'Setting up secure access',
      body: 'Keep the unit on USB power. If asked, enter the six-digit code shown on its screen.',
    },
    needsUsb: {
      face: Severity.Warn,
      title: 'Plug the unit into USB',
      body: 'Plug the unit into USB, then try again. You’ll need to read the code on its screen.',
    },
    firmwareTooOld: {
      face: Severity.Bad,
      title: 'Update the unit’s firmware',
      body:
        'This unit’s firmware is too old for this app. Update the unit’s firmware over USB (see the guide on ' +
        `GitHub: ${FIRMWARE_GUIDE_URL}), then try again.`,
    },
    appTooOld: {
      face: Severity.Bad,
      title: 'Update the app',
      body: 'This unit’s firmware is newer than this app. Update the app, then try again.',
    },
    error: { face: Severity.Bad, title: 'Couldn’t connect', body: error ?? 'Something went wrong.' },
    ready: { face: Severity.Ok, title: 'Paired', body: 'Fetching the unit’s history…' },
  };
  const c = content[phase] ?? content.connecting;
  const waiting = phase === 'connecting' || phase === 'pairing';
  return (
    <View className="gap-5">
      <Card className="items-center gap-4 py-8">
        <Face size={96} severity={c.face} />
        <View className="items-center gap-1.5 px-2">
          <T className="font-body-semi text-lg text-center">{c.title}</T>
          <T className="text-muted text-center leading-6">{c.body}</T>
        </View>
        {waiting && <ActivityIndicator color={t.textMuted} />}
      </Card>
      {phase === 'firmwareTooOld' && (
        <Button title="Open the firmware guide" onPress={() => void Linking.openURL(FIRMWARE_GUIDE_URL).catch(() => {})} />
      )}
      {(phase === 'needsUsb' || phase === 'error' || phase === 'firmwareTooOld') && (
        <Button title="Try again" kind={phase === 'firmwareTooOld' ? 'secondary' : undefined} onPress={onRetry} />
      )}
      {!waiting && phase !== 'ready' && <Button title="Choose another unit" kind="secondary" onPress={onBack} />}
    </View>
  );
}
