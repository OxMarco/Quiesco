// App-level glue: remember the unit, reconnect on launch, sync after connect,
// and tell screens when stored data changed.

import { Appearance } from 'react-native';

import * as db from '@/data/db';
import {
  type CalibrationOffsets,
  type CalibrationState,
  Capability,
  type CoreConfig,
  hasCapability,
  TemperatureUnit,
} from '@/protocol/codec';
import { DEFAULT_SLEEP_WINDOW, type SleepWindow } from '@/ui/sleep';
import { localeTempUnit, type TempUnit } from '@/ui/units';

import * as keys from './keys';
import * as linkApi from './link';
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
  await syncNow().catch(() => {});
}

export async function syncNow() {
  const added = await linkApi.syncLog();
  await reloadUnits();
  session.set((s) => ({ dataVersion: s.dataVersion + 1 }));
  return added;
}

const SLEEP_WINDOW_KEY = 'sleepWindow';

export async function setSleepWindow(w: SleepWindow) {
  await db.setSetting(SLEEP_WINDOW_KEY, w);
  session.set({ sleepWindow: w });
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
  const theme = await db.getSetting<ThemePref>(THEME_KEY);
  if (theme === 'auto' || theme === 'light' || theme === 'dark') {
    applyTheme(theme);
    session.set({ theme });
  }
  await reloadUnits();
  watchUnitSettings();
  ble.onRadioState((radio) => {
    session.set({ radio });
    if (radio === 'on') reconnect();
  });
  const radio = await ble.radioState().catch(() => 'unknown' as const);
  session.set({ radio });
  if (radio === 'on') reconnect();
}

/** Quietly reconnect to the saved unit when nothing else is going on. */
export async function reconnect() {
  const unit = session.get().units[0];
  const phase = linkApi.link.get().phase;
  if (!unit || (phase !== 'idle' && phase !== 'error')) return;
  if (!(await ble.requestPermissions())) return;
  await open(unit.peripheralId);
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
