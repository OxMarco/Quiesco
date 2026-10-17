// App-level glue: remember the units and which one is shown, reconnect on
// launch, sync after connect (the others quietly), and tell screens when
// stored data changed.

import * as Application from 'expo-application';
import * as StoreReview from 'expo-store-review';
import { AppState, Appearance, Platform } from 'react-native';

import * as db from '@/data/db';
import { trace } from '@/debug/trace';
import * as health from '@/health/sleep';
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
import type { SleepSpan } from '@/ui/health-sleep';
import type { SleepWindow } from '@/ui/sleep';
import { localeTempUnit, type TempUnit } from '@/ui/units';

import * as keys from './keys';
import * as linkApi from './link';
import { logEraseUpTo } from './log-erase';
import { shouldAskForReview } from './review-prompt';
import { Store, useStore } from './store';
import * as ble from './transport';
import {
  backgroundSyncTargets,
  DEFAULT_UNIT_PREFS,
  type LegacyPrefs,
  newUnitPrefs,
  pickUnit,
  prefsFromLegacy,
  type UnitPrefs,
} from './units';

/** Day and night follow the phone unless the user picks one. */
export type ThemePref = 'auto' | 'light' | 'dark';

/**
 * What a unit last reported about its own settings, kept so its settings
 * screen can show them greyed out while it is out of range.
 */
export interface UnitCache {
  config: CoreConfig | null;
  offsets: CalibrationOffsets | null;
  calibration: CalibrationState | null;
}

interface SessionState {
  loaded: boolean;
  units: db.Unit[];
  /** The serial of the unit every tab shows; see shownUnit(). */
  selected: string | null;
  /** Each unit's phone-side settings, by serial; see prefsFor(). */
  prefs: Record<string, UnitPrefs>;
  /** Each unit's last known settings, by serial; missing before its first connection. */
  caches: Record<string, UnitCache>;
  /** The unit being synced quietly in the background, while the shown one waits. */
  background: string | null;
  theme: ThemePref;
  /** Bumped after every saved sync, so history screens reload. */
  dataVersion: number;
  radio: ble.RadioState;
  tempUnit: TempUnit;
  /** False until the user has picked °C or °F; tempUnit is then the locale's guess. */
  tempUnitSet: boolean;
  /** Judge nights by the sleep the phone's health store recorded, where it has some. */
  healthSleep: boolean;
}

export const session = new Store<SessionState>({
  loaded: false,
  units: [],
  selected: null,
  prefs: {},
  caches: {},
  background: null,
  theme: 'auto',
  dataVersion: 0,
  radio: 'unknown',
  tempUnit: localeTempUnit(),
  tempUnitSet: false,
  healthSleep: false,
});

export function useSession<S>(select: (s: SessionState) => S): S {
  return useStore(session, select);
}

/** The unit every tab shows: the chosen one, or the first added. */
export function shownUnit(s: SessionState): db.Unit | null {
  return pickUnit(s.units, s.selected);
}

export function useShownUnit(): db.Unit | null {
  return useSession(shownUnit);
}

/** A unit's phone-side settings; the defaults until it has its own. */
export function prefsFor(s: SessionState, serial: string | null | undefined): UnitPrefs {
  return (serial && s.prefs[serial]) || DEFAULT_UNIT_PREFS;
}

/** The shown unit's settings: its sleep window is the one every screen judges by. */
export function useShownPrefs(): UnitPrefs {
  return useSession((s) => prefsFor(s, shownUnit(s)?.serial));
}

const cacheKey = (serial: string) => `unitCache:${serial}`;
const prefsKey = (serial: string) => `unitPrefs:${serial}`;
const SELECTED_KEY = 'selectedUnit';

export async function reloadUnits() {
  const units = await db.listUnits();
  const caches: Record<string, UnitCache> = {};
  const prefs: Record<string, UnitPrefs> = {};
  for (const u of units) {
    const cache = await db.getSetting<UnitCache>(cacheKey(u.serial));
    if (cache) caches[u.serial] = cache;
    const saved = await db.getSetting<UnitPrefs>(prefsKey(u.serial));
    if (saved) prefs[u.serial] = saved;
  }
  session.set({ units, caches, prefs, loaded: true });
}

/** Keep the cache current while connected; writes only when something changed. */
function watchUnitSettings() {
  let last = '';
  linkApi.link.subscribe(() => {
    const l = linkApi.link.get();
    if (l.phase !== 'ready' || !l.serial || !l.config) return;
    const cache: UnitCache = { config: l.config, offsets: l.offsets, calibration: l.calibration };
    const json = JSON.stringify({ serial: l.serial, cache });
    if (json === last) return;
    last = json;
    const serial = l.serial;
    session.set((s) => ({ caches: { ...s.caches, [serial]: cache } }));
    db.setSetting(cacheKey(serial), cache).catch(() => {});
  });
}

async function savePrefs(serial: string, next: UnitPrefs) {
  await db.setSetting(prefsKey(serial), next);
  session.set((s) => ({ prefs: { ...s.prefs, [serial]: next } }));
}

// The phone-wide settings saved before units had their own; the first unit
// added after an upgrade inherits them.
let legacyPrefs: UnitPrefs | null = null;
const LEGACY_KEYS = ['sleepWindow', 'panelFollowsSleep', 'eraseAfterSync'] as const;

/** A unit seen for the first time takes the shown unit's settings (or the old phone-wide ones). */
async function ensurePrefs(serial: string) {
  const s = session.get();
  if (s.prefs[serial]) return;
  const shown = shownUnit(s);
  const template = shown && shown.serial !== serial && s.prefs[shown.serial] ? s.prefs[shown.serial] : legacyPrefs;
  await savePrefs(serial, newUnitPrefs(template));
}

/** Show this unit on every tab, and connect to it unless it already is. */
export async function selectUnit(serial: string) {
  session.set({ selected: serial });
  await db.setSetting(SELECTED_KEY, serial);
  const unit = shownUnit(session.get());
  const l = linkApi.link.get();
  if (!unit || (l.serial === serial && l.phase === 'ready')) return;
  await reconnect();
}

/** Connect, then pull the log so the history is current. */
export async function open(peripheralId: string) {
  await linkApi.connect(peripheralId);
  const serial = linkApi.link.get().serial;
  if (linkApi.link.get().phase !== 'ready' || !serial) return;
  await reloadUnits();
  await ensurePrefs(serial);
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
  // Never over a quiet background sync: the user is looking at another unit.
  if (!session.get().background) await askForReviewIfDue().catch((e) => trace('review prompt failed', e));
  if (AppState.currentState !== 'active') await settleInBackground().catch((e) => trace('settle failed', e));
  return count;
}

const LAST_REVIEW_ASK_KEY = 'lastReviewAskMs';

/** After a complete sync, now and then, the system's rating prompt. */
async function askForReviewIfDue() {
  const l = linkApi.link.get();
  if (!l.serial || AppState.currentState !== 'active') return;
  const lastAskedMs = await db.getSetting<number>(LAST_REVIEW_ASK_KEY);
  const nights = (await db.nightsWithData(l.serial, 60)).length;
  const installed = await Application.getInstallationTimeAsync().catch(() => null);
  const installedMs = installed ? installed.getTime() : null;
  if (!shouldAskForReview({ syncDone: l.sync?.state === 'done', nights, installedMs, lastAskedMs, nowMs: Date.now() })) return;
  // False on TestFlight and the web.
  if (!(await StoreReview.isAvailableAsync())) return;
  await db.setSetting(LAST_REVIEW_ASK_KEY, Date.now());
  await StoreReview.requestReview();
}

const LAST_LOG_ERASE_KEY = 'lastLogEraseMs';
const lastEraseKey = (serial: string) => `${LAST_LOG_ERASE_KEY}:${serial}`;

/** After a complete sync, have the unit erase what the phone now holds. */
async function eraseLogIfDue() {
  const l = linkApi.link.get();
  if (l.phase !== 'ready' || !l.serial || !l.info || !l.status) return;
  if (!prefsFor(session.get(), l.serial).eraseAfterSync) return;
  const unit = await db.getUnit(l.serial);
  // Before units had their own, the last erase was saved for the phone.
  const lastEraseMs = (await db.getSetting<number>(lastEraseKey(l.serial))) ?? (await db.getSetting<number>(LAST_LOG_ERASE_KEY));
  const upTo = logEraseUpTo({
    enabled: true,
    capable: hasCapability(l.info, Capability.logEraseCommand),
    syncDone: l.sync?.state === 'done',
    cursor: unit?.cursor ?? 0,
    newestSequence: l.status.newestSequence,
    lastEraseMs,
    nowMs: Date.now(),
  });
  if (upTo === null) return;
  trace('log erase', upTo);
  await linkApi.eraseLogUpTo(upTo);
  await db.setSetting(lastEraseKey(l.serial), Date.now());
}

/** Phone-side only: takes effect after the unit's next complete sync. */
export async function setEraseAfterSync(serial: string, on: boolean) {
  await savePrefs(serial, { ...prefsFor(session.get(), serial), eraseAfterSync: on });
}

export async function setSleepWindow(serial: string, w: SleepWindow) {
  await savePrefs(serial, { ...prefsFor(session.get(), serial), sleepWindow: w });
  // In the background: saving on the phone is what the user asked for, and
  // the next connect pushes it again anyway.
  pushSleepWindow().catch((e) => trace('sleep window push failed', e));
}

/** Saved on the phone, then written to the unit if it is connected; otherwise on the next connect. */
export async function setPanelFollowsSleep(serial: string, on: boolean) {
  await savePrefs(serial, { ...prefsFor(session.get(), serial), panelFollowsSleep: on });
  await pushSleepWindow();
}

/** The connected unit's panel uses its own sleep window, in the phone's time zone. */
async function pushSleepWindow() {
  const l = linkApi.link.get();
  if (l.phase !== 'ready' || !l.serial || !l.info || !hasCapability(l.info, Capability.sleepWindow)) return;
  const { sleepWindow, panelFollowsSleep } = prefsFor(session.get(), l.serial);
  const want: UnitSleepWindow = {
    followSleep: panelFollowsSleep,
    utcOffsetMin: -new Date().getTimezoneOffset(),
    bedMin: sleepWindow.times.bedMin,
    wakeMin: sleepWindow.times.wakeMin,
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

const HEALTH_SLEEP_KEY = 'healthSleep';

/** Turning it on asks the health store for access first; stays off if that fails. */
export async function setHealthSleep(on: boolean): Promise<boolean> {
  const granted = on ? await health.requestSleepAccess() : false;
  await db.setSetting(HEALTH_SLEEP_KEY, granted);
  session.set((s) => ({ healthSleep: granted, dataVersion: s.dataVersion + 1 }));
  return granted;
}

/** Recorded sleep over [fromMs, toMs), or none while the setting is off. */
export async function recordedSleep(fromMs: number, toMs: number): Promise<SleepSpan[]> {
  if (!session.get().healthSleep) return [];
  return health.sleepSpans(fromMs, toMs);
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

let loading: Promise<void> | null = null;

/**
 * The stored part of the session: settings and units, no Bluetooth. A
 * background run (no screen) needs just this; boot() adds the listeners.
 */
export function loadSession(): Promise<void> {
  loading ??= readSession().catch((e) => {
    loading = null;
    throw e;
  });
  return loading;
}

async function readSession() {
  const unit = await db.getSetting<TempUnit>(TEMP_UNIT_KEY);
  if (unit === 'C' || unit === 'F') session.set({ tempUnit: unit, tempUnitSet: true });
  const theme = await db.getSetting<ThemePref>(THEME_KEY);
  if (theme === 'auto' || theme === 'light' || theme === 'dark') {
    applyTheme(theme);
    session.set({ theme });
  }
  const selected = await db.getSetting<string>(SELECTED_KEY);
  if (typeof selected === 'string') session.set({ selected });
  if ((await db.getSetting<boolean>(HEALTH_SLEEP_KEY)) === true) session.set({ healthSleep: true });
  await reloadUnits();
  await migrateLegacyPrefs();
}

let booted = false;

/** Called once from the root layout. */
export async function boot() {
  if (booted) return;
  booted = true;
  await loadSession();
  watchUnitSettings();
  ble.onRadioState((radio) => {
    session.set({ radio });
    if (radio === 'on') quietReconnect();
  });
  // A link that drops while the app is in the background (the unit restarts,
  // or walks out of range) gets the same treatment as leaving the screen.
  let wasReady = false;
  linkApi.link.subscribe(() => {
    const ready = linkApi.link.get().phase === 'ready';
    if (wasReady && !ready && AppState.currentState !== 'active') settleInBackground().catch(() => {});
    wasReady = ready;
  });
  AppState.addEventListener('change', (state) => {
    // Back from the background: the unit may be in range again.
    if (state === 'active' && session.get().radio === 'on') quietReconnect();
    if (state === 'background') settleInBackground().catch((e) => trace('settle failed', e));
  });
  const radio = await ble.radioState().catch(() => 'unknown' as const);
  session.set({ radio });
  if (radio === 'on') quietReconnect();
}

/**
 * Before units had their own settings, the sleep window, the panel choice and
 * the log erase were the phone's. Units already on the phone keep them; the
 * next unit added inherits them if none is there yet.
 */
async function migrateLegacyPrefs() {
  const [sleepWindow, panelFollowsSleep, eraseAfterSync] = await Promise.all(LEGACY_KEYS.map((k) => db.getSetting<unknown>(k)));
  const legacy = prefsFromLegacy({ sleepWindow: sleepWindow as LegacyPrefs['sleepWindow'], panelFollowsSleep, eraseAfterSync });
  if (!legacy) return;
  const { units, prefs } = session.get();
  if (units.length === 0) {
    legacyPrefs = legacy;
    return;
  }
  for (const u of units) if (!prefs[u.serial]) await savePrefs(u.serial, legacy);
  for (const k of LEGACY_KEYS) await db.setSetting(k, null);
}

function quietReconnect() {
  // Not while other units sync in the background: that ends on the shown unit anyway.
  if (session.get().background) return;
  reconnect()
    .then(() => syncOthersIfDue())
    .catch((e) => trace('reconnect failed', e));
}

// Phases where nothing is under way, so a reconnect may start one. needsUsb
// and the version mismatches end an attempt too: the user may have plugged
// the unit in or updated it since.
const SETTLED: linkApi.Phase[] = ['idle', 'error', 'needsUsb', 'firmwareTooOld', 'appTooOld'];
let reconnecting: Promise<void> | null = null;

/** Connect to the shown unit when nothing else is going on; ends a background sync first. */
export function reconnect(): Promise<void> {
  // Claimed synchronously, so radio, foreground and boot can all ask at once.
  reconnecting ??= (async () => {
    stopBackgroundSync();
    const unit = shownUnit(session.get());
    if (!unit) return;
    const l = linkApi.link.get();
    if (l.serial === unit.serial && !SETTLED.includes(l.phase)) return;
    if (!(await ble.requestPermissions())) return;
    await open(unit.peripheralId);
  })().finally(() => {
    reconnecting = null;
  });
  return reconnecting;
}

// ------------------------------------------------- other units, quietly

const IN_RANGE_SCAN_S = 5;
let backgroundGeneration = 0;

/** Stop syncing other units; the one under way is dropped by the next connect. */
export function stopBackgroundSync() {
  backgroundGeneration++;
  if (session.get().background) session.set({ background: null });
}

/** Which of these peripherals advertise nearby, from a short scan. */
async function inRange(ids: string[], live: () => boolean): Promise<Set<string>> {
  const seen = new Set<string>();
  const want = new Set(ids);
  const stop = await ble.scan((a) => {
    if (want.has(a.id)) seen.add(a.id);
  }, IN_RANGE_SCAN_S);
  await new Promise((resolve) => setTimeout(resolve, IN_RANGE_SCAN_S * 1000));
  // Stopped meanwhile, the radio may be scanning for the Add screen: leave it.
  stop(live());
  return seen;
}

/**
 * The phone holds one connection at a time. After the shown unit has synced,
 * visit each other unit in range that has not synced for a while, then come
 * back to the shown one, so switching finds every unit's nights ready.
 */
async function syncOthersIfDue() {
  const gen = ++backgroundGeneration;
  const live = () => gen === backgroundGeneration;
  const s = session.get();
  const shown = shownUnit(s);
  const others = s.units.filter((u) => u.serial !== shown?.serial);
  if (others.length === 0) return;
  // A unit without a key on this phone would ask for a setup code: never in the background.
  const candidates = [];
  for (const u of others) candidates.push({ ...u, quiet: (await keys.loadKey(u.serial)) !== null });
  // Scan only when some unit is due, so coming back to the app is free most times.
  const due = backgroundSyncTargets(candidates, shown?.serial ?? null, new Set(others.map((u) => u.peripheralId)), Date.now());
  if (due.length === 0 || !live()) return;
  const nearby = await inRange(due.map((u) => u.peripheralId), live).catch(() => new Set<string>());
  const targets = backgroundSyncTargets(due, shown?.serial ?? null, nearby, Date.now());
  if (targets.length === 0 || !live()) return;
  trace('background sync', targets.map((u) => u.serial));
  try {
    for (const u of targets) {
      if (!live()) return;
      session.set({ background: u.serial });
      await open(u.peripheralId).catch((e) => trace('background sync failed', u.serial, e));
    }
  } finally {
    if (live()) {
      session.set({ background: null });
      const back = shownUnit(session.get());
      if (back) await open(back.peripheralId).catch((e) => trace('reconnect failed', e));
    }
  }
}

/** Forget a unit on this phone: its readings, its settings and the saved link. */
export async function forget(serial: string) {
  stopBackgroundSync();
  if (linkApi.link.get().serial === serial) await linkApi.disconnect();
  await keys.deleteKey(serial);
  await db.forgetUnit(serial);
  await db.setSetting(cacheKey(serial), null);
  await db.setSetting(prefsKey(serial), null);
  await db.setSetting(lastEraseKey(serial), null);
  await reloadUnits();
  session.set((s) => ({
    dataVersion: s.dataVersion + 1,
    // The next unit along takes its place on every tab.
    selected: s.selected === serial ? (s.units[0]?.serial ?? null) : s.selected,
  }));
  await db.setSetting(SELECTED_KEY, session.get().selected);
  quietReconnect();
}

// --------------------------------------------------- in the background

/**
 * The unit's battery decides what the app does when it leaves the screen. On
 * USB power the link stays up, so live readings keep the widgets current; on
 * battery it is dropped, because a held connection drains the unit, and the
 * next background run syncs again. Out of range, the system is asked to connect
 * whenever the unit comes back (waitFor), which wakes the app to sync.
 */
async function settleInBackground() {
  if (AppState.currentState === 'active') return;
  const unit = shownUnit(session.get());
  if (!unit) return;
  const l = linkApi.link.get();
  if (l.serial === unit.serial && l.phase === 'ready') {
    const syncing = l.sync && l.sync.state !== 'done' && l.sync.state !== 'failed';
    if (syncing) return; // syncNow settles when it ends
    if (!unitOnUsb()) await linkApi.disconnect();
    return;
  }
  if (l.serial === unit.serial && !SETTLED.includes(l.phase)) return; // still connecting
  waitForUnit(unit);
}

/** On USB power (or a bench unit with no battery), holding the link costs the unit nothing. */
function unitOnUsb(): boolean {
  const info = linkApi.link.get().info;
  return !!info && (info.noBattery || info.pairingOpen);
}

let waitingFor: string | null = null;

function waitForUnit(unit: db.Unit) {
  if (Platform.OS === 'web' || waitingFor === unit.peripheralId) return;
  waitingFor = unit.peripheralId;
  trace('waiting for unit', unit.serial);
  ble
    .waitFor(unit.peripheralId)
    .then(() => backgroundSync('unit in range'))
    .catch(() => {})
    .finally(() => {
      if (waitingFor === unit.peripheralId) waitingFor = null;
    });
}

/**
 * One background run: sync the shown unit if it can be reached, which also
 * sets its clock. Never asks for anything (permissions, a setup code), so a
 * unit without a key on this phone is skipped. Called by the periodic task
 * (expo-background-task) and when a waited-for unit comes into range.
 */
export async function backgroundSync(reason: string): Promise<void> {
  trace('background sync', reason);
  await loadSession();
  const unit = shownUnit(session.get());
  if (!unit || (await keys.loadKey(unit.serial)) === null) return;
  if (!(await ble.hasPermissions())) return;
  if ((await ble.radioState().catch(() => 'unknown')) !== 'on') return;
  const l = linkApi.link.get();
  if (l.serial === unit.serial && l.phase === 'ready') await syncNow().catch((e) => trace('background sync failed', e));
  else await (reconnecting ?? open(unit.peripheralId)).catch((e) => trace('background sync failed', e));
  if (AppState.currentState !== 'active') await settleInBackground().catch(() => {});
}
