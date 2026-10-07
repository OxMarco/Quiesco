// Settings: the app's own first, then the unit's. The unit's settings live on
// the unit, so out of range they show what it last reported, greyed out.

import * as Application from 'expo-application';
import Constants from 'expo-constants';
import { router } from 'expo-router';
import * as WebBrowser from 'expo-web-browser';
import { useEffect, useRef, useState } from 'react';
import { Alert, Linking, TextInput, View } from 'react-native';

import * as linkApi from '@/ble/link';
import { link } from '@/ble/link';
import {
  forget,
  reconnect,
  reloadUnits,
  setEraseAfterSync,
  setPanelFollowsSleep,
  setTempUnit,
  setTheme,
  type ThemePref,
  useSession,
} from '@/ble/session';
import { useStore } from '@/ble/store';
import { Segmented, Stepper } from '@/components/controls';
import { DiagnosticReportRow } from '@/components/diagnostic-report';
import { ScreenPicker } from '@/components/screen-picker';
import { TempUnitPicker } from '@/components/temp-unit';
import { Button, Card, Row, Screen, SectionLabel, T, Title } from '@/components/ui';
import { isDemo } from '@/data/sample';
import { SUPPORT_EMAIL } from '@/debug/report';
import {
  CalibrationOffsets,
  Capability,
  deviceNameProblem,
  hasCapability,
  INTERVALS_S,
  IntervalSeconds,
  STATUS_DEVICES,
} from '@/protocol/codec';
import { useTheme } from '@/theme';
import { ago, intervalLabel } from '@/ui/format';
import { formatClock } from '@/ui/sleep';
import { deltaFromUnit, deltaToUnit, tempSymbol, type TempUnit } from '@/ui/units';

export default function UnitScreen() {
  const unit = useSession((s) => s.units[0] ?? null);
  const cache = useSession((s) => s.unitCache);
  const tempUnit = useSession((s) => s.tempUnit);
  const panelFollowsSleep = useSession((s) => s.panelFollowsSleep);
  const eraseAfterSync = useSession((s) => s.eraseAfterSync);
  const s = useStore(link, (x) => x);
  // Each failed write reports next to the control that caused it.
  const [error, setError] = useState<{ at: ErrorAt; message: string } | null>(null);
  // The unit confirms the write at once but repaints e-ink a second or two
  // later, and never says when it is done; set expectations instead of spinning.
  const [screenApplying, setScreenApplying] = useState(false);
  const screenTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => {
    if (screenTimer.current) clearTimeout(screenTimer.current);
  }, []);

  const connected = unit !== null && s.phase === 'ready' && s.serial === unit.serial;
  const connecting = s.phase === 'connecting' || s.phase === 'pairing';
  const can = (bit: number) => connected && s.info !== null && hasCapability(s.info, bit);
  const run = (at: ErrorAt, action: () => Promise<unknown>) => {
    setError(null);
    return action().catch((e) => {
      setError({ at, message: linkApi.describeError(e) });
      throw e;
    });
  };
  const errorAt = (at: ErrorAt) => (error?.at === at ? error.message : null);
  // Live values while connected, the last reported ones otherwise.
  const config = connected ? s.config : cache?.config ?? null;
  const offsets = connected ? s.offsets : cache?.offsets ?? null;
  const calibration = connected ? s.calibration : cache?.calibration ?? null;
  const demo = isDemo(unit);
  // Shown only while connected: there is no cached value to grey out.
  const showPanelSleep = can(Capability.sleepWindow);
  const showRefresh = connected && s.config !== null;
  const showEraseLog = can(Capability.logEraseCommand);

  const toggleEraseLog = (on: boolean) => {
    if (on === eraseAfterSync) return;
    if (!on) return void run('eraseLog', () => setEraseAfterSync(false)).catch(() => {});
    Alert.alert(
      'Erase the unit’s log after syncing?',
      'Once every record is on this phone, the unit erases its own copy, at most once a day. Your history then lives only on this phone: if you lose or reset it, those nights are gone.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Turn on', style: 'destructive', onPress: () => void run('eraseLog', () => setEraseAfterSync(true)).catch(() => {}) },
      ],
    );
  };

  return (
    <Screen>
      <Title>Settings</Title>

      <SectionLabel>App</SectionLabel>
      <AppSettings />

      <SectionLabel>Device</SectionLabel>
      {!unit ? (
        <Card className="gap-4">
          <T className="text-muted leading-6">No unit on this phone yet.</T>
          <Button title="Add your Quiesco" onPress={() => router.push('/connect')} />
        </Card>
      ) : demo ? (
        <>
          <Card className="py-1">
            <Row label="Name" value={unit.name} />
            <T className="text-[13px] text-muted leading-[18px] pb-3.5">
              Demo data, made up on this phone. There is no unit to connect to, so its settings are hidden. Remove the demo
              to add your Quiesco.
            </T>
          </Card>
          <View className="h-6" />
          <DangerZone serial={unit.serial} connected={false} canErase={false} demo />
        </>
      ) : (
        <>
          {!connected && (
            <Card className="flex-row items-center gap-3">
              <T className="text-sm text-muted leading-5 shrink grow">
                {connecting ? 'Connecting to the unit…' : 'Out of range or asleep. Its settings change on the unit itself, so connect to edit them.'}
              </T>
              <View className="shrink-0">
                <Button title="Connect" kind="secondary" busy={connecting} onPress={() => reconnect()} />
              </View>
            </Card>
          )}
          <Card className="py-1">
            <NameRow
              name={s.name ?? unit.name}
              disabled={!can(Capability.rename)}
              onSave={(name) => run('name', () => linkApi.setName(name).then(reloadUnits))}
              error={errorAt('name')}
            />
            <Field
              label="Screen"
              hint="What the e-ink screen shows. Bento has the biggest numbers; Face shows a smile until something is wrong."
              error={errorAt('screen')}>
              <ScreenPicker
                disabled={!can(Capability.displayScreen)}
                value={config?.displayScreen ?? null}
                onChange={(v) =>
                  void run('screen', async () => {
                    await linkApi.setScreen(v);
                    if (screenTimer.current) clearTimeout(screenTimer.current);
                    setScreenApplying(true);
                    screenTimer.current = setTimeout(() => setScreenApplying(false), 3000);
                  }).catch(() => {})
                }
              />
              {screenApplying && <T className="text-sm text-muted leading-5">The unit’s screen updates in a moment.</T>}
            </Field>
            <Field
              label="Temperature"
              hint={
                connected && !can(Capability.temperatureUnit)
                  ? 'Used in the app. This unit’s firmware always shows °C on its screen.'
                  : 'Used in the app and on the unit’s screen.'
              }
              error={errorAt('tempUnit')}>
              <TempUnitPicker
                value={tempUnit}
                onChange={(v) => void run('tempUnit', () => setTempUnit(v)).catch(() => {})}
              />
            </Field>
            <Field label="Measure every" hint="More often gives finer charts and shorter battery life." error={errorAt('interval')}>
              <Segmented<IntervalSeconds>
                disabled={!connected}
                value={(config?.intervalSeconds as IntervalSeconds) ?? null}
                onChange={(v) => void run('interval', () => linkApi.setMeasurementInterval(v)).catch(() => {})}
                options={INTERVALS_S.map((v) => ({ value: v, label: intervalLabel(v) }))}
              />
            </Field>
            <Field
              label="Bluetooth"
              hint="“Plugged in” lets the phone connect only while the unit is on USB power, and saves battery."
              error={errorAt('bluetooth')}
              last={!showPanelSleep && !showRefresh && !showEraseLog}>
              <Segmented<'always' | 'plugged'>
                disabled={!connected}
                value={config ? (config.bleAlwaysAvailable ? 'always' : 'plugged') : null}
                onChange={(v) => void run('bluetooth', () => linkApi.setAlwaysAvailable(v === 'always')).catch(() => {})}
                options={[
                  { value: 'always', label: 'Always' },
                  { value: 'plugged', label: 'Plugged in' },
                ]}
              />
            </Field>
            {showPanelSleep && (
              <Field
                label="Panel follows your sleep window"
                hint="By day the unit only flags CO₂ and loud noise, like the app."
                error={errorAt('panelSleep')}
                last={!showRefresh && !showEraseLog}>
                <Segmented<'on' | 'off'>
                  label="Panel follows your sleep window"
                  value={panelFollowsSleep ? 'on' : 'off'}
                  onChange={(v) => void run('panelSleep', () => setPanelFollowsSleep(v === 'on')).catch(() => {})}
                  options={[
                    { value: 'on', label: 'On' },
                    { value: 'off', label: 'Off' },
                  ]}
                />
              </Field>
            )}
            {showRefresh && s.config && (
              <Field
                label="Deep refresh"
                hint="Every so many screen updates. More often clears ghosting; less often flashes less and saves battery."
                error={errorAt('refresh')}
                last={!showEraseLog}>
                <Segmented<number>
                  label="Deep refresh, every so many screen updates"
                  value={s.config.fullRefreshEvery}
                  onChange={(v) => void run('refresh', () => linkApi.setFullRefreshEvery(v)).catch(() => {})}
                  options={refreshOptions(s.config.fullRefreshEvery)}
                />
              </Field>
            )}
            {showEraseLog && (
              <Field
                label="Erase the unit’s log after syncing"
                hint="Your history then lives only on this phone. At most once a day, and only once every record is here."
                error={errorAt('eraseLog')}
                last>
                <Segmented<'on' | 'off'>
                  label="Erase the unit’s log after syncing"
                  value={eraseAfterSync ? 'on' : 'off'}
                  onChange={(v) => toggleEraseLog(v === 'on')}
                  options={[
                    { value: 'off', label: 'Off' },
                    { value: 'on', label: 'On' },
                  ]}
                />
              </Field>
            )}
          </Card>

          <SectionLabel>Calibration</SectionLabel>
          <Card className="py-1">
            {offsets ? (
              <OffsetsForm
                key={`${JSON.stringify(offsets)}${connected}`}
                offsets={offsets}
                tempUnit={tempUnit}
                disabled={!can(Capability.calibrationOffsets)}
                onSave={(o) => void run('offsets', () => linkApi.setOffsets(o)).catch(() => {})}
                error={errorAt('offsets')}
              />
            ) : (
              <T className="text-sm text-muted leading-5 py-3.5 border-b border-line">
                Offsets for temperature, humidity and noise appear here once the unit has connected.
              </T>
            )}
            <Row
              label="Recalibrate CO₂"
              value={
                calibration?.lastFrcReferencePpm
                  ? `Last ${calibration.lastFrcEpochS ? ago(calibration.lastFrcEpochS * 1000) : 'done'}`
                  : 'Never'
              }
              onPress={can(Capability.frc) ? () => router.push('/calibrate') : undefined}
              last
            />
          </Card>

          <SectionLabel>Unit details</SectionLabel>
          <Card className="py-1">
            <Row label="Firmware" value={s.firmware ?? unit.firmware ?? '--'} />
            <Row label="Serial" value={unit.serial} last={!(s.status && connected)} />
            {s.status && connected && (
              <SensorsRow presentMask={s.status.presentMask} failures={s.status.failures} noBattery={s.info?.noBattery ?? false} />
            )}
          </Card>

          <View className="h-6" />
          <DangerZone serial={unit.serial} connected={can(Capability.factoryReset)} canErase={can(Capability.logErase)} />
        </>
      )}

      <SectionLabel>About</SectionLabel>
      <About />
    </Screen>
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
      <DiagnosticReportRow />
      {error && <T className="text-[13px] text-bad leading-[18px] pt-3">{error}</T>}
      <T className="text-[13px] text-muted leading-[18px] py-3.5">
        Quiesco is not a medical device. Its readings and advice are for general comfort information only.
      </T>
    </Card>
  );
}

/** What lives on the phone only: the sleep window and the theme. */
function AppSettings() {
  const sleep = useSession((s) => s.sleepWindow);
  const theme = useSession((s) => s.theme);
  return (
    <Card className="py-1">
      <Row
        label="Sleep window"
        value={`${formatClock(sleep.weekday.bedMin)} – ${formatClock(sleep.weekday.wakeMin)}`}
        onPress={() => router.push('/sleep-window')}
      />
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

type ErrorAt = 'name' | 'screen' | 'tempUnit' | 'interval' | 'bluetooth' | 'panelSleep' | 'refresh' | 'eraseLog' | 'offsets';

const REFRESH_EVERY = [5, 10, 20, 50];

/** The usual choices, plus the unit's own value if it was set to something else. */
function refreshOptions(current: number) {
  const values = REFRESH_EVERY.includes(current) ? REFRESH_EVERY : [...REFRESH_EVERY, current].sort((a, b) => a - b);
  return values.map((v) => ({ value: v, label: String(v) }));
}

function Field({
  label,
  hint,
  error,
  children,
  last,
}: {
  label: string;
  hint?: string;
  error?: string | null;
  children: React.ReactNode;
  last?: boolean;
}) {
  return (
    <View className={`py-3.5 gap-2.5 ${last ? '' : 'border-b border-line'}`}>
      <T className="text-[15px]">{label}</T>
      {children}
      {hint && <T className="text-[13px] text-muted leading-[18px]">{hint}</T>}
      {error && <T className="text-[13px] text-bad leading-[18px]">{error}</T>}
    </View>
  );
}

/** The unit's name, edited in place. The unit caps it at 16 bytes. */
function NameRow({
  name,
  disabled,
  onSave,
  error,
}: {
  name: string;
  disabled: boolean;
  onSave: (name: string) => Promise<unknown>;
  error: string | null;
}) {
  const t = useTheme();
  const [draft, setDraft] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  if (draft === null) {
    return (
      <>
        <Row label="Name" value={name} onPress={disabled ? undefined : () => setDraft(name)} last={!error} />
        {error && <T className="text-[13px] text-bad leading-[18px] pb-3 border-b border-line">{error}</T>}
      </>
    );
  }
  const trimmed = draft.trim();
  const problem = trimmed === name ? null : deviceNameProblem(trimmed);
  const save = () => {
    if (trimmed === name) return setDraft(null);
    setBusy(true);
    onSave(trimmed)
      .then(() => setDraft(null))
      .catch(() => {})
      .finally(() => setBusy(false));
  };
  return (
    <View className="py-3.5 gap-2.5 border-b border-line">
      <T className="text-[15px]">Name</T>
      <TextInput
        value={draft}
        onChangeText={setDraft}
        autoFocus
        maxLength={16}
        returnKeyType="done"
        onSubmitEditing={() => !problem && save()}
        selectionColor={t.primary}
        className="h-11 rounded-xl bg-raised px-3 font-body text-[15px] text-ink"
      />
      {(problem || error) && <T className="text-[13px] text-bad leading-[18px]">{problem ?? error}</T>}
      <View className="flex-row gap-3">
        <View className="flex-1">
          <Button title="Cancel" kind="secondary" onPress={() => setDraft(null)} />
        </View>
        <View className="flex-1">
          <Button title="Save" busy={busy} disabled={!!problem} onPress={save} />
        </View>
      </View>
    </View>
  );
}

function OffsetsForm({
  offsets,
  tempUnit,
  disabled,
  onSave,
  error,
}: {
  offsets: CalibrationOffsets;
  tempUnit: TempUnit;
  disabled: boolean;
  onSave: (o: CalibrationOffsets) => void;
  error: string | null;
}) {
  const [draft, setDraft] = useState(offsets);
  const changed =
    draft.temperatureC !== offsets.temperatureC || draft.humidityPct !== offsets.humidityPct || draft.noiseDb !== offsets.noiseDb;
  return (
    <View className="py-3.5 gap-1 border-b border-line">
      <T className="text-sm text-muted leading-5 mb-1">
        Compare with a trusted reference and add the difference. Set temperature first: correcting it also corrects
        humidity.
      </T>
      <Stepper
        label="Temperature"
        unit={tempSymbol(tempUnit)}
        step={tempUnit === 'F' ? 0.2 : 0.1}
        min={-deltaToUnit(8, tempUnit)}
        max={deltaToUnit(8, tempUnit)}
        value={Math.round(deltaToUnit(draft.temperatureC, tempUnit) * 10) / 10}
        onChange={(v) => setDraft({ ...draft, temperatureC: deltaFromUnit(v, tempUnit) })}
        disabled={disabled}
      />
      <Stepper label="Humidity" unit="%" step={0.5} min={-20} max={20} value={draft.humidityPct} onChange={(v) => setDraft({ ...draft, humidityPct: v })} disabled={disabled} />
      <Stepper label="Noise" unit="dB" step={0.5} min={-24} max={24} value={draft.noiseDb} onChange={(v) => setDraft({ ...draft, noiseDb: v })} disabled={disabled} />
      {changed && (
        <View className="flex-row gap-3 mt-2">
          <View className="flex-1">
            <Button title="Discard" kind="secondary" onPress={() => setDraft(offsets)} />
          </View>
          <View className="flex-1">
            <Button title="Apply" onPress={() => onSave(draft)} />
          </View>
        </View>
      )}
      {error && <T className="text-[13px] text-bad leading-[18px] mt-1">{error}</T>}
    </View>
  );
}

function SensorsRow({ presentMask, failures, noBattery }: { presentMask: number; failures: number[]; noBattery: boolean }) {
  // A bench build without a cell has no battery monitor by design.
  const problems = STATUS_DEVICES.map((name, i) => ({ name, present: (presentMask >> i) & 1, failures: failures[i] })).filter(
    (d) => !(noBattery && d.name === 'Battery monitor') && (!d.present || d.failures > 0),
  );
  if (problems.length === 0) return <Row label="Sensors" value="All working" last />;
  const details = problems
    .map((p) => `${p.name}: ${p.present ? `failed ${p.failures} times in a row` : 'not detected'}`)
    .join('\n');
  return (
    <Row
      label="Sensors"
      value={
        <T className="text-bad text-[15px] font-body-semi">
          {problems.length === 1 ? '1 problem' : `${problems.length} problems`}
        </T>
      }
      onPress={() => Alert.alert('Sensor problems', details)}
      last
    />
  );
}

function DangerZone({
  serial,
  connected,
  canErase,
  demo = false,
}: {
  serial: string;
  connected: boolean;
  canErase: boolean;
  /** The demo unit: removing it only deletes the made-up nights. */
  demo?: boolean;
}) {
  // A message replaces the rows while something runs or after a reset; a
  // failure keeps the rows so the user can try again.
  const [done, setDone] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  if (done) {
    return (
      <Card>
        <T className="text-sm leading-5">{done}</T>
      </Card>
    );
  }

  const doForget = () => {
    setError(null);
    setDone(demo ? 'Removing the demo…' : 'Forgetting the unit…');
    // On success the unit is gone and this screen shows "No unit" instead.
    forget(serial)
      .then(() => setDone(null))
      .catch((e) => {
        setDone(null);
        setError(`Could not ${demo ? 'remove the demo' : 'forget the unit'}: ${linkApi.describeError(e)}`);
      });
  };

  const confirmForget = () =>
    demo
      ? Alert.alert('Remove demo data?', 'The made-up nights are deleted from this phone.', [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Remove', style: 'destructive', onPress: doForget },
        ])
      : Alert.alert(
          'Forget this phone?',
          'Nights synced to this phone are deleted from it, and anything the unit has not synced yet will no longer reach this phone. The unit keeps its own log.',
          [
            { text: 'Cancel', style: 'cancel' },
            { text: 'Forget', style: 'destructive', onPress: doForget },
          ],
        );

  const reset = (erase: boolean) => {
    setError(null);
    return linkApi
      .factoryReset(erase)
      .then(() =>
        setDone(
          'The unit is back to its factory settings and has forgotten every phone, and this phone has dropped its key. To use it again, plug the unit into USB power and add it again in the app.',
        ),
      )
      .catch((e) => setError(linkApi.describeError(e)));
  };

  const confirmReset = () =>
    Alert.alert(
      'Reset the unit?',
      'This restores its settings, name and calibration offsets and unpairs every phone. Sync first: nights not yet on this phone may be lost. CO₂ recalibration is kept.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Reset, keep the log', style: 'destructive', onPress: () => void reset(false) },
        ...(canErase
          ? [{ text: 'Reset and erase the log', style: 'destructive' as const, onPress: () => void reset(true) }]
          : []),
      ],
    );

  if (demo) {
    return (
      <Card className="py-1">
        <Row label="Remove demo data" danger onPress={confirmForget} last={!error} />
        {error && <T className="text-[13px] text-bad leading-[18px] pb-3">{error}</T>}
      </Card>
    );
  }

  return (
    <Card className="py-1">
      <Row label="Forget this phone" danger onPress={confirmForget} />
      <Row
        label="Reset unit"
        danger={connected}
        onPress={connected ? confirmReset : undefined}
        value={connected ? undefined : 'Connect first'}
        last={!error}
      />
      {error && <T className="text-[13px] text-bad leading-[18px] pb-3">{error}</T>}
    </Card>
  );
}
