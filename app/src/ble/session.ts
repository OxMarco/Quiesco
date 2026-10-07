// App-level glue: remember the unit, reconnect on launch, sync after connect,
// and tell screens when stored data changed.

import { AppState, Appearance } from 'react-native';

import * as db from '@/data/db';
import { isDemo } from '@/data/sample';
import { trace } from '@/debug/trace';
import {
  type CalibrationOffsets,
  type CalibrationState,
  Capability,
  type CoreConfig,
  hasCapability,
  sameSleepWindow,
  TemperatureUnit,
  type UnitSleepWindow,
} from '@/protocol/codec';
import { DEFAULT_SLEEP_WINDOW, type SleepWindow } from '@/ui/sleep';
import { localeTempUnit, type TempUnit } from '@/ui/units';

import * as keys from './keys';
import * as linkApi from './link';
import { logEraseUpTo } from './log-erase';
import { Store, useStore } from './store';
import * as ble from './transport';

/** Day and night follow the phone unless the user picks one. */
export type ThemePref = 'auto' | 'light' | 'dark';

/**
 * What the unit last reported about its own settings, kept so the Unit tab
 * can show them greyed out while the unit is out of range.
 */
export interface UnitCache {
  config: CoreConfig | null;
  offsets: CalibrationOffsets | null;
  calibration: CalibrationState | null;
}

interface SessionState {
  loaded: boolean;
  units: db.Unit[];
  /** The first unit's last known settings, or null before its first connection. */
  unitCache: UnitCache | null;
  theme: ThemePref;
  /** Bumped after every saved sync, so history screens reload. */
  dataVersion: number;
  radio: ble.RadioState;
  sleepWindow: SleepWindow;
  tempUnit: TempUnit;
  /** False until the user has picked °C or °F; tempUnit is then the locale's guess. */
  tempUnitSet: boolean;
  /** The unit's panel judges by time of day like the app (capability sleepWindow). */
  panelFollowsSleep: boolean;
  /** Erase the unit's log once it is all on this phone (capability logEraseCommand). */
  eraseAfterSync: boolean;
}

export const session = new Store<SessionState>({
  loaded: false,
  units: [],
  unitCache: null,
  theme: 'auto',
  dataVersion: 0,
  radio: 'unknown',
  sleepWindow: DEFAULT_SLEEP_WINDOW,
  tempUnit: localeTempUnit(),
  tempUnitSet: false,
  panelFollowsSleep: true,
  eraseAfterSync: false,
});

export function useSession<S>(select: (s: SessionState) => S): S {
  return useStore(session, select);
}

const cacheKey = (serial: string) => `unitCache:${serial}`;

export async function reloadUnits() {
  const units = await db.listUnits();
  const unitCache = units[0] ? await db.getSetting<UnitCache>(cacheKey(units[0].serial)) : null;
  session.set({ units, unitCache, loaded: true });
}

/** Keep the cache current while connected; writes only when something changed. */
function watchUnitSettings() {
  let last = '';
  linkApi.link.subscribe(() => {
    const l = linkApi.link.get();
    if (l.phase !== 'ready' || !l.serial || !l.config) return;
    const cache: UnitCache = { config: l.config, offsets: l.offsets, calibration: l.calibration };
    const json = JSON.stringify(cache);
    if (json === last) return;
    last = json;
    session.set({ unitCache: cache });
    db.setSetting(cacheKey(l.serial), cache).catch(() => {});
  });
}

/** Connect, then pull the log so the history is current. */
export async function open(peripheralId: string) {
  await linkApi.connect(peripheralId);
  if (linkApi.link.get().phase !== 'ready') return;
  await reloadUnits();
  await pushTempUnit().catch(() => {});
  // Every connect, so the unit's UTC offset follows the phone across time zones.
  await pushSleepWindow().catch((e) => trace('sleep window push failed', e));
  await syncNow().catch(() => {});
}

export async function syncNow() {
  let count: number;
  try {
    count = await linkApi.syncLog();
  } finally {
    // A failed sync can still have saved part of the log.
    await reloadUnits().catch(() => {});
    session.set((s) => ({ dataVersion: s.dataVersion + 1 }));
  }
  await eraseLogIfDue().catch((e) => trace('log erase failed', e));
  return count;
}

const LAST_LOG_ERASE_KEY = 'lastLogEraseMs';

/** After a complete sync, have the unit erase what the phone now holds. */
async function eraseLogIfDue() {
  const l = linkApi.link.get();
  if (!session.get().eraseAfterSync || l.phase !== 'ready' || !l.serial || !l.info || !l.status) return;
  const unit = await db.getUnit(l.serial);
  const upTo = logEraseUpTo({
    enabled: true,
    capable: hasCapability(l.info, Capability.logEraseCommand),
    syncDone: l.sync?.state === 'done',
    cursor: unit?.cursor ?? 0,
    newestSequence: l.status.newestSequence,
    lastEraseMs: await db.getSetting<number>(LAST_LOG_ERASE_KEY),
    nowMs: Date.now(),
  });
  if (upTo === null) return;
  trace('log erase', upTo);
  await linkApi.eraseLogUpTo(upTo);
  await db.setSetting(LAST_LOG_ERASE_KEY, Date.now());
}

const ERASE_AFTER_SYNC_KEY = 'eraseAfterSync';

/** Phone-side only: takes effect after the next complete sync. */
export async function setEraseAfterSync(on: boolean) {
  await db.setSetting(ERASE_AFTER_SYNC_KEY, on);
  session.set({ eraseAfterSync: on });
}

const SLEEP_WINDOW_KEY = 'sleepWindow';

export async function setSleepWindow(w: SleepWindow) {
  await db.setSetting(SLEEP_WINDOW_KEY, w);
  session.set({ sleepWindow: w });
  // In the background: saving on the phone is what the user asked for, and
  // the next connect pushes it again anyway.
  pushSleepWindow().catch((e) => trace('sleep window push failed', e));
}

const PANEL_FOLLOWS_SLEEP_KEY = 'panelFollowsSleep';

/** Saved on the phone, then written to the unit if it is connected; otherwise on the next connect. */
export async function setPanelFollowsSleep(on: boolean) {
  await db.setSetting(PANEL_FOLLOWS_SLEEP_KEY, on);
  session.set({ panelFollowsSleep: on });
  await pushSleepWindow();
}

/** The unit's panel uses the phone's sleep window, in the phone's time zone. */
async function pushSleepWindow() {
  const l = linkApi.link.get();
  if (l.phase !== 'ready' || !l.info || !hasCapability(l.info, Capability.sleepWindow)) return;
  const { sleepWindow, panelFollowsSleep } = session.get();
  const want: UnitSleepWindow = {
    followSleep: panelFollowsSleep,
    utcOffsetMin: -new Date().getTimezoneOffset(),
    weekdayBedMin: sleepWindow.weekday.bedMin,
    weekdayWakeMin: sleepWindow.weekday.wakeMin,
    weekendBedMin: sleepWindow.weekend?.bedMin ?? null,
    weekendWakeMin: sleepWindow.weekend?.wakeMin ?? null,
  };
  if (l.sleepWindow && sameSleepWindow(l.sleepWindow, want)) return;
  await linkApi.setUnitSleepWindow(want);
}

const TEMP_UNIT_KEY = 'tempUnit';

/** Saved on the phone, then written to the unit if it is connected; otherwise on the next connect. */
export async function setTempUnit(unit: TempUnit) {
  await db.setSetting(TEMP_UNIT_KEY, unit);
  session.set({ tempUnit: unit, tempUnitSet: true });
  await pushTempUnit();
}

/** The phone's setting wins: the unit's screen follows whatever the app shows. */
async function pushTempUnit() {
  const l = linkApi.link.get();
  if (l.phase !== 'ready' || !l.config || !l.info || !hasCapability(l.info, Capability.temperatureUnit)) return;
  const want = session.get().tempUnit === 'F' ? TemperatureUnit.Fahrenheit : TemperatureUnit.Celsius;
  if (l.config.temperatureUnit !== want) await linkApi.setTemperatureUnit(want);
}

const THEME_KEY = 'theme';

function applyTheme(pref: ThemePref) {
  Appearance.setColorScheme(pref === 'auto' ? 'unspecified' : pref);
}

export async function setTheme(pref: ThemePref) {
  applyTheme(pref);
  await db.setSetting(THEME_KEY, pref);
  session.set({ theme: pref });
}

let booted = false;

/** Called once from the root layout. */
export async function boot() {
  if (booted) return;
  booted = true;
  const saved = await db.getSetting<SleepWindow>(SLEEP_WINDOW_KEY);
  if (saved) session.set({ sleepWindow: saved });
  const unit = await db.getSetting<TempUnit>(TEMP_UNIT_KEY);
  if (unit === 'C' || unit === 'F') session.set({ tempUnit: unit, tempUnitSet: true });
  const follows = await db.getSetting<boolean>(PANEL_FOLLOWS_SLEEP_KEY);
  if (typeof follows === 'boolean') session.set({ panelFollowsSleep: follows });
  const erase = await db.getSetting<boolean>(ERASE_AFTER_SYNC_KEY);
  if (typeof erase === 'boolean') session.set({ eraseAfterSync: erase });
  const theme = await db.getSetting<ThemePref>(THEME_KEY);
  if (theme === 'auto' || theme === 'light' || theme === 'dark') {
    applyTheme(theme);
    session.set({ theme });
  }
  await reloadUnits();
  watchUnitSettings();
  ble.onRadioState((radio) => {
    session.set({ radio });
    if (radio === 'on') quietReconnect();
  });
  // Back from the background: the unit may be in range again.
  AppState.addEventListener('change', (state) => {
    if (state === 'active' && session.get().radio === 'on') quietReconnect();
  });
  const radio = await ble.radioState().catch(() => 'unknown' as const);
  session.set({ radio });
  if (radio === 'on') quietReconnect();
}

function quietReconnect() {
  reconnect().catch((e) => trace('reconnect failed', e));
}

// Phases where nothing is under way, so a reconnect may start one. needsUsb
// and the version mismatches end an attempt too: the user may have plugged
// the unit in or updated it since.
const SETTLED: linkApi.Phase[] = ['idle', 'error', 'needsUsb', 'firmwareTooOld', 'appTooOld'];
let reconnecting: Promise<void> | null = null;

/** Quietly reconnect to the saved unit when nothing else is going on. */
export function reconnect(): Promise<void> {
  // Claimed synchronously, so radio, foreground and boot can all ask at once.
  reconnecting ??= (async () => {
    const unit = session.get().units[0];
    // The demo unit has no radio behind it.
    if (!unit || isDemo(unit) || !SETTLED.includes(linkApi.link.get().phase)) return;
    if (!(await ble.requestPermissions())) return;
    await open(unit.peripheralId);
  })().finally(() => {
    reconnecting = null;
  });
  return reconnecting;
}

/** Forget the unit on this phone: its readings and the saved link. */
export async function forget(serial: string) {
  await linkApi.disconnect();
  await keys.deleteKey(serial);
  await db.forgetUnit(serial);
  await db.setSetting(cacheKey(serial), null);
  await reloadUnits();
  session.set((s) => ({ dataVersion: s.dataVersion + 1 }));
}
