import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { ActivityIndicator, Linking, Pressable, View } from 'react-native';

import { APP_STORE_URL, describeError, FIRMWARE_GUIDE_URL, link } from '@/ble/link';
import { scanner, startScan, stopScan } from '@/ble/scanner';
import { open, selectUnit, session, stopBackgroundSync, useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import * as ble from '@/ble/transport';
import { Face } from '@/components/face';
import { Button, Card, Sheet, SheetHeader, T } from '@/components/ui';
import { Severity } from '@/protocol/comfort';
import { useTheme } from '@/theme';

export default function ConnectScreen() {
  const t = useTheme();
  const phase = useStore(link, (s) => s.phase);
  const error = useStore(link, (s) => s.error);
  const radio = useSession((s) => s.radio);
  // Adding a unit is step one of three: its name and its sleep window follow.
  const firstRun = useSession((s) => s.units.length === 0);
  const units = useSession((s) => s.units);
  // Units on the phone when the sheet opened: finding one again only reconnects it.
  const [known] = useState(() => new Set(session.get().units.map((u) => u.serial)));
  const found = useStore(scanner, (s) => s.found);
  const scanning = useStore(scanner, (s) => s.scanning);
  const permission = useStore(scanner, (s) => s.permitted);
  const [chosen, setChosen] = useState<ble.Advert | null>(null);
  // Failures after the link is up (saving the unit, say) never reach the link's own error.
  const [openError, setOpenError] = useState<string | null>(null);

  useEffect(() => {
    // The radio is needed here: no quiet sync of other units meanwhile.
    stopBackgroundSync();
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
    const serial = link.get().serial;
    if (chosen && phase === 'ready' && !openError && serial) {
      // A new unit is shown from now on, then named and given its sleep window.
      const next = () => {
        selectUnit(serial).catch(() => {});
        if (known.has(serial)) router.back();
        else router.replace({ pathname: '/name-unit', params: { serial } });
      };
      const timer = setTimeout(next, 900);
      return () => clearTimeout(timer);
    }
  }, [chosen, phase, openError, known]);

  return (
    <Sheet className="px-4 pt-6 pb-10 gap-5">
      <SheetHeader
        title={firstRun ? 'Add your Quiesco' : 'Add another Quiesco'}
        eyebrow="Step 1 of 3"
        action="Close"
        onAction={() => router.back()}
      />

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
        <Card>
          <T className="leading-6">
            Bluetooth is off. Turn it on in Settings or Control Center; the list fills in on its own once it’s on.
          </T>
        </Card>
      ) : radio === 'unauthorized' || permission === false ? (
        <View className="gap-3">
          <Card><T className="leading-6">Quiesco needs Bluetooth permission to find your unit. Allow it in Settings, then come back.</T></Card>
          <Button title="Open Settings" onPress={() => void Linking.openSettings().catch(() => {})} />
        </View>
      ) : (
        <>
          <T className="text-muted leading-6">
            Keep the unit nearby and plug it into USB power. During setup, enter the six-digit code shown on the unit’s
            screen.
          </T>
          {found.length === 0 ? (
            <Searching scanning={scanning} />
          ) : (
            <>
              <View className="gap-2">
                {found.map((a) => {
                  const added = units.find((u) => u.peripheralId === a.id);
                  if (added) return <AddedUnit key={a.id} name={added.name} />;
                  return (
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
                  );
                })}
              </View>
              {scanning ? (
                <View className="flex-row items-center gap-3 px-1">
                  <ActivityIndicator color={t.textMuted} />
                  <T className="text-muted">Looking for more…</T>
                </View>
              ) : (
                <Button title="Search again" kind="secondary" onPress={startScan} />
              )}
            </>
          )}
        </>
      )}
    </Sheet>
  );
}

/** A unit already on this phone: shown so the list makes sense, not offered again. */
function AddedUnit({ name }: { name: string }) {
  return (
    <Card className="flex-row items-center gap-4 opacity-60" accessible accessibilityLabel={`${name}, already on this phone`}>
      <Face size={40} />
      <View className="flex-1">
        <T className="font-body-semi text-base">{name}</T>
        <T className="text-[13px] text-muted">Already on this phone</T>
      </View>
    </Card>
  );
}

/** The sheet while the list is empty: the sleeping face, and the wait or what to do about it. */
function Searching({ scanning }: { scanning: boolean }) {
  const t = useTheme();
  return (
    <View className="gap-5">
      <Card className="items-center gap-4 py-8">
        <Face size={96} />
        <View className="items-center gap-1.5 px-2">
          <T className="font-body-semi text-lg text-center">{scanning ? 'Looking for units…' : 'No units found'}</T>
          <T className="text-muted text-center leading-6">
            {scanning
              ? 'A unit on USB power shows up within a few seconds.'
              : 'If the unit is set to connect only after measuring, plug it into USB or wait for its next measurement.'}
          </T>
        </View>
        {scanning && <ActivityIndicator color={t.textMuted} />}
      </Card>
      {!scanning && <Button title="Search again" kind="secondary" onPress={startScan} />}
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
      body: 'This unit’s firmware is too old for this app. Update it over USB with the guide on GitHub, then add the unit again.',
    },
    appTooOld: {
      face: Severity.Bad,
      title: 'Update the app',
      body: 'This unit’s firmware is newer than this app. Update the app from the store, then add the unit again.',
    },
    error: { face: Severity.Bad, title: 'Couldn’t connect', body: error ?? 'Something went wrong.' },
    ready: { face: Severity.Ok, title: 'Paired', body: 'This phone can read the unit from now on.' },
  };
  const c = content[phase] ?? content.connecting;
  const waiting = phase === 'connecting' || phase === 'pairing';
  const openUrl = (url: string) => () => void Linking.openURL(url).catch(() => {});
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
      {/* A version mismatch won't clear on retry: offer the fix, not another attempt. */}
      {phase === 'firmwareTooOld' && <Button title="Open the firmware guide" onPress={openUrl(FIRMWARE_GUIDE_URL)} />}
      {phase === 'appTooOld' && <Button title="Update the app" onPress={openUrl(APP_STORE_URL)} />}
      {(phase === 'needsUsb' || phase === 'error') && <Button title="Try again" onPress={onRetry} />}
      {!waiting && phase !== 'ready' && <Button title="Choose another unit" kind="secondary" onPress={onBack} />}
    </View>
  );
}
