// Settings: the app's own first, then the units on this phone, each opening
// its own settings, then About.

import * as Application from 'expo-application';
import Constants from 'expo-constants';
import { router } from 'expo-router';
import * as WebBrowser from 'expo-web-browser';
import { useState } from 'react';
import { Linking } from 'react-native';

import * as linkApi from '@/ble/link';
import { link } from '@/ble/link';
import { setHealthSleep, setTempUnit, setTheme, type ThemePref, useSession } from '@/ble/session';
import { useStore } from '@/ble/store';
import { Segmented } from '@/components/controls';
import { DiagnosticReportRow } from '@/components/diagnostic-report';
import { TempUnitPicker } from '@/components/temp-unit';
import { Field } from '@/components/unit-settings';
import { Button, Card, Row, Screen, SectionLabel, T, Title } from '@/components/ui';
import type * as db from '@/data/db';
import { SUPPORT_EMAIL } from '@/debug/report';
import { HEALTH_STORE } from '@/health/sleep';
import { Capability, hasCapability } from '@/protocol/codec';
import { ago } from '@/ui/format';
import type { TempUnit } from '@/ui/units';

export default function SettingsScreen() {
  const units = useSession((s) => s.units);
  const s = useStore(link, (x) => x);
  const [tempUnitError, setTempUnitError] = useState<string | null>(null);
  const ready = s.phase === 'ready' && s.info !== null;

  const onTempUnit = (v: TempUnit) => {
    setTempUnitError(null);
    setTempUnit(v).catch((e) => setTempUnitError(linkApi.describeError(e)));
  };

  return (
    <Screen tabBar>
      <Title>Settings</Title>

      <SectionLabel>App</SectionLabel>
      <AppSettings
        tempUnitHint={ready && !hasCapability(s.info!, Capability.temperatureUnit) ? 'This unit’s screen always shows °C' : undefined}
        tempUnitError={tempUnitError}
        onTempUnit={onTempUnit}
      />

      <SectionLabel>{units.length > 1 ? 'Your units' : 'Your unit'}</SectionLabel>
      {units.length === 0 ? (
        <Card className="gap-4">
          <T className="text-muted leading-6">No unit on this phone yet</T>
          <Button title="Add your Quiesco" onPress={() => router.push('/connect')} />
        </Card>
      ) : (
        <Card className="py-1">
          {units.map((u) => (
            <UnitRow key={u.serial} unit={u} />
          ))}
          <Row label="Add another Quiesco" onPress={() => router.push('/connect')} last />
        </Card>
      )}

      <SectionLabel>About</SectionLabel>
      <About />
    </Screen>
  );
}

/** One unit in the list: its name and whether the phone can reach it. */
function UnitRow({ unit }: { unit: db.Unit }) {
  const connected = useStore(link, (s) => s.phase === 'ready' && s.serial === unit.serial);
  const syncing = useSession((s) => s.background === unit.serial);
  const status = connected
      ? 'Connected'
      : syncing
        ? 'Syncing…'
        : unit.lastSyncMs
          ? `Synced ${ago(unit.lastSyncMs)}`
          : 'Not synced yet';
  return (
    <Row label={unit.name} value={status} onPress={() => router.push({ pathname: '/units/[serial]', params: { serial: unit.serial } })} />
  );
}

const PRIVACY_URL = 'https://quiesco.rest/privacy';

/** The app itself: always shown, paired or not (App Review looks for it). */
function About() {
  const version = Constants.expoConfig?.version ?? '--';
  // The binary's own build number; null in Expo Go.
  const build = Application.nativeBuildVersion;
  const [error, setError] = useState<string | null>(null);
  const openOrSay = (p: Promise<unknown>, what: string) => {
    setError(null);
    p.catch(() => setError(`Couldn’t open ${what}. Write to ${SUPPORT_EMAIL} instead.`));
  };
  return (
    <Card className="py-1">
      <Row label="Version" value={build ? `${version} (${build})` : version} />
      <Row label="Privacy policy" onPress={() => openOrSay(WebBrowser.openBrowserAsync(PRIVACY_URL), 'the privacy policy')} />
      <Row label="Support" value={SUPPORT_EMAIL} onPress={() => openOrSay(Linking.openURL(`mailto:${SUPPORT_EMAIL}`), 'your mail app')} />
      <Row label="Open-source licences" onPress={() => router.push('/licences')} />
      <DiagnosticReportRow />
      {error && <T className="text-[13px] text-bad leading-[18px] pt-3">{error}</T>}
      <T className="text-[13px] text-muted leading-[18px] py-3.5">
        Quiesco is not a medical device. Its readings and advice are for general comfort information only.
      </T>
    </Card>
  );
}

/** What the phone keeps for every unit: the temperature unit (which the units' screens follow) and the theme. */
function AppSettings({
  tempUnitHint,
  tempUnitError,
  onTempUnit,
}: {
  tempUnitHint?: string;
  tempUnitError: string | null;
  onTempUnit: (v: TempUnit) => void;
}) {
  const theme = useSession((s) => s.theme);
  const tempUnit = useSession((s) => s.tempUnit);
  const healthSleep = useSession((s) => s.healthSleep);
  const [healthNote, setHealthNote] = useState<string | null>(null);
  const onHealth = (on: boolean) => {
    setHealthNote(null);
    setHealthSleep(on)
      .then((granted) => {
        if (on && !granted) setHealthNote(`${HEALTH_STORE} didn’t allow Quiesco to read sleep`);
      })
      .catch(() => setHealthNote(`Couldn’t reach ${HEALTH_STORE}`));
  };
  return (
    <Card className="py-1">
      <Field label="Temperature" hint={tempUnitHint} error={tempUnitError}>
        <TempUnitPicker value={tempUnit} onChange={onTempUnit} />
      </Field>
      {HEALTH_STORE && (
        <Field
          label={`Sleep times from ${HEALTH_STORE}`}
          hint="Nights are judged by when you actually slept, where it has a record. Read only, and it stays on this phone."
          error={healthNote}>
          <Segmented<'on' | 'off'>
            value={healthSleep ? 'on' : 'off'}
            onChange={(v) => onHealth(v === 'on')}
            options={[
              { value: 'on', label: 'On' },
              { value: 'off', label: 'Off' },
            ]}
          />
        </Field>
      )}
      <Field label="Theme" last>
        <Segmented<ThemePref>
          value={theme}
          onChange={(v) => setTheme(v)}
          options={[
            { value: 'auto', label: 'Auto' },
            { value: 'light', label: 'Day' },
            { value: 'dark', label: 'Night' },
          ]}
        />
      </Field>
    </Card>
  );
}
