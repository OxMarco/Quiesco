// One live connection to a Quiesco unit: the connect sequence (PROTOCOL.md §2),
// app-layer auth (§3), confirmed writes (§5) and log download (§7).

import * as db from '@/data/db';
import {
  CalibrationOffsets,
  CalibrationState,
  Capability,
  CoreConfig,
  decodeCalibrationOffsets,
  decodeCalibrationState,
  decodeAuthState,
  decodeCoreConfig,
  decodeEnrolKey,
  decodeDeviceInfo,
  decodeDeviceName,
  decodeDiagnostics,
  decodeReading,
  decodeSleepWindow,
  decodeStatus,
  DeviceInfo,
  Diagnostics,
  DisplayScreen,
  encodeCalibrationOffsets,
  encodeCoreConfig,
  encodeDeviceName,
  encodeDisplayScreen,
  encodeEnrol,
  encodeProve,
  encodeSleepWindow,
  EnrolledKey,
  encodeEpoch,
  encodeFactoryReset,
  encodeFrc,
  encodeLogErase,
  encodeLogSyncAbort,
  encodeLogSyncStart,
  hasCapability,
  IntervalSeconds,
  sameSleepWindow,
  Measurement,
  Status,
  TemperatureUnit,
  UnitSleepWindow,
  utf8Decode,
} from '@/protocol/codec';
import { LogSession, logWasReplaced } from '@/protocol/log';
import { Chr, Dis, DIS_SERVICE, PROTOCOL_VERSION, QUIESCO_SERVICE } from '@/protocol/uuids';

import { trace } from '@/debug/trace';

import * as keys from './keys';
import { cancelEnrollment, requestSetupKey } from './enrollment';

import { Store } from './store';
import * as ble from './transport';

/** How to flash new firmware over USB; shown to units on an older protocol. */
export const FIRMWARE_GUIDE_URL = 'https://github.com/OxMarco/Quiesco#-build-your-own';

export type Phase =
  | 'idle'
  | 'connecting'
  | 'pairing' // signing in or enrolling at the app layer (§3)
  | 'needsUsb' // no usable key and the unit is not on USB power (§3)
  | 'firmwareTooOld' // the unit speaks an older protocol: update its firmware
  | 'appTooOld' // the unit speaks a newer protocol: update the app
  | 'ready'
  | 'error';

export interface SyncProgress {
  state: 'waiting' | 'streaming' | 'saving' | 'done' | 'failed';
  received: number;
  remaining: number | null;
  /** Records the phone lacks when the sync began; an estimate, null if unknown. */
  expected: number | null;
  message?: string;
}

export interface LinkState {
  phase: Phase;
  peripheralId: string | null;
  serial: string | null;
  name: string | null;
  info: DeviceInfo | null;
  firmware: string | null;
  reading: Measurement | null;
  readingAt: number | null;
  status: Status | null;
  config: CoreConfig | null;
  offsets: CalibrationOffsets | null;
  calibration: CalibrationState | null;
  diagnostics: Diagnostics | null;
  /** The sleep window the panel uses (capability sleepWindow). */
  sleepWindow: UnitSleepWindow | null;
  sync: SyncProgress | null;
  error: string | null;
}

const initial: LinkState = {
  phase: 'idle',
  peripheralId: null,
  serial: null,
  name: null,
  info: null,
  firmware: null,
  reading: null,
  readingAt: null,
  status: null,
  config: null,
  offsets: null,
  calibration: null,
  diagnostics: null,
  sleepWindow: null,
  sync: null,
  error: null,
};

export const link = new Store<LinkState>(initial);

// ------------------------------------------------------------------ helpers

function withTimeout<T>(promise: Promise<T>, ms: number, what: string): Promise<T> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${what} timed out`)), ms);
    promise.then(
      (v) => {
        clearTimeout(timer);
        resolve(v);
      },
      (e) => {
        clearTimeout(timer);
        reject(e);
      },
    );
  });
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// What the connect sequence needs before encryption (§2): device info and the
// DIS serial and firmware revision.
const REQUIRED = ['000C', '2A25', '2A26'];

// The device can take ~5 s to answer while the panel refreshes (§5).
const REQUEST_MS = 8_000;

let payload = 20;
let unsubscribers: (() => void)[] = [];
let disconnectSub: { remove(): void } | null = null;
let userDisconnect = false;

function id(): string {
  const p = link.get().peripheralId;
  if (!p) throw new Error('No unit connected');
  return p;
}

const readQ = (chr: string) => withTimeout(ble.read(id(), QUIESCO_SERVICE, chr), REQUEST_MS, 'Read');
const writeQ = (chr: string, data: Uint8Array) =>
  withTimeout(ble.write(id(), QUIESCO_SERVICE, chr, data), REQUEST_MS, 'Write');
const readDis = async (chr: string) => utf8Decode(await withTimeout(ble.read(id(), DIS_SERVICE, chr), REQUEST_MS, 'Read'));

function can(bit: number): boolean {
  const info = link.get().info;
  return info !== null && hasCapability(info, bit);
}

// --------------------------------------------------------------- connecting

// One connect attempt at a time. A newer attempt (or a disconnect) bumps the
// generation and waits for the older one to unwind before touching the radio,
// so the older one's failure handling never tears down the newer link.
let generation = 0;
let inFlight: { peripheralId: string; promise: Promise<void> } | null = null;

class Superseded extends Error {
  constructor() {
    super('Superseded by a newer connection attempt.');
  }
}

/**
 * Connect and run the recommended sequence (§2). Resolves once the link is
 * ready, or leaves the phase at needsUsb / firmwareTooOld / appTooOld / error. A second
 * call for the same unit while one is running joins it.
 */
export function connect(peripheralId: string): Promise<void> {
  if (inFlight?.peripheralId === peripheralId) return inFlight.promise;
  const gen = ++generation;
  const previous = inFlight?.promise;
  const promise = (async () => {
    if (previous) {
      await teardown();
      await previous.catch(() => {});
    }
    if (gen === generation) await runConnect(peripheralId, gen);
  })().finally(() => {
    if (inFlight?.promise === promise) inFlight = null;
  });
  inFlight = { peripheralId, promise };
  return promise;
}

async function runConnect(peripheralId: string, gen: number): Promise<void> {
  const guard = () => {
    if (gen !== generation) throw new Superseded();
  };
  await teardown();
  userDisconnect = false;
  link.set({ ...initial, phase: 'connecting', peripheralId });
  trace('connect', peripheralId);
  try {
    const offered = await withTimeout(ble.connect(peripheralId), 15_000, 'Connecting');
    guard();
    const missing = REQUIRED.filter((c) => !offered.includes(c));
    if (missing.length > 0) {
      trace('missing characteristics', missing);
      link.set({
        phase: 'error',
        error:
          `The unit doesn’t offer ${missing.join(', ')}. It offers: ${offered.join(', ') || 'nothing'}. ` +
          'If you connected before a firmware update, forget “Quiesco” in the iPhone’s Bluetooth settings, ' +
          'turn Bluetooth off and on, then try again. Otherwise update the unit’s firmware.',
      });
      await ble.disconnect(peripheralId);
      return;
    }
    disconnectSub = ble.onDisconnect((dropped) => {
      if (dropped === peripheralId) onDropped();
    });
    payload = await ble.negotiatePayload(peripheralId);
    trace('att payload', payload);

    // 1–3: compatibility and identity, readable without encryption.
    const info = decodeDeviceInfo(await readQ(Chr.deviceInfo));
    const serial = await readDis(Dis.serial);
    const firmware = await readDis(Dis.firmware);
    link.set({ info, serial, firmware });
    trace('identity', { serial, firmware, info });
    if (info.protocolVersion !== PROTOCOL_VERSION) {
      const tooOld = info.protocolVersion < PROTOCOL_VERSION;
      link.set({
        phase: tooOld ? 'firmwareTooOld' : 'appTooOld',
        error: tooOld
          ? `This unit’s firmware is too old for this app. Update the unit’s firmware over USB (see the guide on GitHub: ${FIRMWARE_GUIDE_URL}).`
          : 'This unit’s firmware is newer than this app. Update the app, then try again.',
      });
      await ble.disconnect(peripheralId);
      return;
    }

    // 4: access, authenticated at the app layer.
    await authenticate(serial);
    guard();
    if (link.get().phase === 'needsUsb') return;

    // 5–7: clock, live values, config.
    await writeQ(Chr.epoch, encodeEpoch(Date.now()));
    await subscribeLive();
    await refreshAll();

    const name = link.get().name ?? `Quiesco ${serial.slice(-4)}`;
    guard();
    await db.upsertUnit({ serial, peripheralId, name, firmware });
    link.set({ phase: 'ready' });
    trace('ready');
  } catch (e) {
    trace('connect failed', e);
    // A newer attempt waits for this one, so the radio is still ours to drop;
    // the state is not.
    if (gen === generation && link.get().phase !== 'needsUsb') {
      link.set({ phase: 'error', error: describeError(e) });
    }
    await ble.disconnect(peripheralId);
  }
}

// --------------------------------------------------------- app-layer auth

// The unit applies auth writes from its main loop, which can stall for a few
// seconds (a flash write or a display refresh), so wait generously.
const AUTH_WAIT_MS = 12_000;
const AUTH_POLL_MS = 250;

/**
 * Authenticate this connection (PROTOCOL.md §3): enrol once on USB power,
 * then prove the stored key against the unit's challenge. Leaves the phase at
 * needsUsb when the unit must be plugged in; throws on other failures.
 */
async function authenticate(serial: string) {
  link.set({ phase: 'pairing' });
  let enrolled = await keys.loadKey(serial);
  let refusals = 0;
  // Rounds: the stored key, once more on a fresh challenge, then enrolment.
  for (let round = 0; round < 3; round++) {
    let state = decodeAuthState(await readQ(Chr.auth));
    trace('auth state', { ...state, challenge: undefined, haveKey: enrolled !== null });
    const isNewKey = !enrolled;
    if (!enrolled) {
      if (!state.enrolmentOpen) {
        link.set({ phase: 'needsUsb' });
        await ble.disconnect(id());
        return;
      }
      enrolled = await enrol();
      state = decodeAuthState(await readQ(Chr.auth));
    }
    if (await prove(enrolled, state.challenge)) {
      // Do not retain mistyped or unconfirmed setup codes. The code only
      // admits setup: the unit then hands over the phone's own key.
      if (isNewKey) {
        enrolled = await readIssuedKey(enrolled.keyId);
        await keys.saveKey(serial, enrolled);
      }
      trace('authenticated', { keyId: enrolled.keyId });
      link.set({ phase: 'connecting' });
      return;
    }
    if (isNewKey) throw new Error('The code was rejected or expired. Reconnect and enter the new code shown on the unit.');
    // One refusal can be the unit's doing (it rotates the challenge on its
    // own too), so try the stored key once more before giving it up.
    if (++refusals < 2) {
      trace('key refused, retrying', { keyId: enrolled.keyId });
      continue;
    }
    // Refused twice: the unit no longer knows this key (factory reset, or
    // evicted by a fifth phone). Forget it and enrol again if on USB.
    trace('key rejected', { keyId: enrolled.keyId });
    await keys.deleteKey(serial);
    enrolled = null;
  }
  link.set({ phase: 'needsUsb' });
  await ble.disconnect(id());
}

/** Start setup and return the setup key derived from the code on screen. */
async function enrol(): Promise<EnrolledKey> {
  await writeQ(Chr.auth, encodeEnrol());
  const deadline = Date.now() + AUTH_WAIT_MS;
  while (Date.now() < deadline) {
    await sleep(AUTH_POLL_MS);
    const bytes = await readQ(Chr.enrolKey);
    // While setup is pending 0011 holds only the key id (§6.16).
    if (bytes.length !== 20 || bytes.slice(2).some((b) => b !== 0)) {
      throw new Error('Invalid setup response from the unit.');
    }
    const keyId = bytes[0] | (bytes[1] << 8);
    if (keyId !== 0) return { keyId, key: await requestSetupKey(keyId) };
  }
  throw new Error('The unit did not issue a key. Keep it on USB power and try again.');
}

// The unit shows the issued key for 30 s (§6.16); keep reading for most of it.
const ISSUED_KEY_WAIT_MS = 25_000;

/**
 * The phone key the unit places in 0011 once the setup code is proved. If it
 * never arrives the unit has stored a key this phone lacks; a fresh setup on
 * USB power issues a new key id, and the orphaned one ages out of the unit's
 * table like any other phone.
 */
async function readIssuedKey(keyId: number): Promise<EnrolledKey> {
  const deadline = Date.now() + ISSUED_KEY_WAIT_MS;
  while (Date.now() < deadline) {
    const issued = decodeEnrolKey(await readQ(Chr.enrolKey));
    if (issued && issued.keyId === keyId && issued.key.some((b) => b !== 0)) return issued;
    await sleep(AUTH_POLL_MS);
  }
  throw new Error(
    'The unit accepted the code but this phone didn’t receive its key, so it can’t sign in yet. ' +
      'Keep the unit on USB power and tap Try again to set it up with a new code.',
  );
}

/** True when the unit accepted the proof; false when it rejected the key. */
function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  return a.byteLength === b.byteLength && a.every((v, i) => v === b[i]);
}

async function prove(enrolled: EnrolledKey, challenge: Uint8Array): Promise<boolean> {
  if (challenge.every((b) => b === 0)) throw new Error('The unit has no challenge ready. Try again.');
  const message = encodeProve(enrolled.keyId, enrolled.key, challenge);
  await writeQ(Chr.auth, message);
  const deadline = Date.now() + AUTH_WAIT_MS;
  while (Date.now() < deadline) {
    await sleep(AUTH_POLL_MS);
    const raw = await readQ(Chr.auth);
    // ArduinoBLE serves the bytes just written until the unit's main loop
    // answers (measured on air). Read as a state, our own PROVE would look
    // like a refusal and cost the phone its key.
    if (sameBytes(raw, message)) continue;
    const state = decodeAuthState(raw);
    if (state.authenticated) return true;
    if (state.challenge.every((b) => b === 0)) throw new Error('The unit has no challenge ready. Try again.');
    // A new challenge without the flag: the proof was checked and refused.
    if (state.challenge.some((b, i) => b !== challenge[i])) return false;
  }
  throw new Error('The unit did not answer the sign-in. Try again.');
}

async function subscribeLive() {
  const peripheralId = id();
  unsubscribers.push(
    await ble.subscribe(peripheralId, QUIESCO_SERVICE, Chr.reading, (data) =>
      link.set({ reading: decodeReading(data), readingAt: Date.now() }),
    ),
    await ble.subscribe(peripheralId, QUIESCO_SERVICE, Chr.status, (data) => {
      const status = decodeStatus(data);
      link.set({ status });
      statusListeners.forEach((l) => l(status));
    }),
  );
}

/** Read every value once (§5: after a reconnect, read rather than wait). */
export async function refreshAll() {
  const reading = decodeReading(await readQ(Chr.reading));
  const status = decodeStatus(await readQ(Chr.status));
  const config = decodeCoreConfig(await readQ(Chr.coreConfig));
  const patch: Partial<LinkState> = { reading, readingAt: reading.validMask ? Date.now() : null, status, config };
  if (can(Capability.rename)) patch.name = decodeDeviceName(await readQ(Chr.deviceName));
  if (can(Capability.calibrationOffsets)) patch.offsets = decodeCalibrationOffsets(await readQ(Chr.calibrationOffsets));
  if (can(Capability.calibrationState)) patch.calibration = decodeCalibrationState(await readQ(Chr.calibrationState));
  if (can(Capability.diagnostics)) patch.diagnostics = decodeDiagnostics(await readQ(Chr.diagnostics));
  if (can(Capability.sleepWindow)) patch.sleepWindow = decodeSleepWindow(await readQ(Chr.sleepWindow));
  link.set(patch);
}

function onDropped() {
  cancelEnrollment('The unit disconnected. Reconnect to get a new setup code.');
  const wasSyncing = link.get().sync?.state === 'waiting' || link.get().sync?.state === 'streaming';
  unsubscribers.forEach((u) => u());
  unsubscribers = [];
  link.set((s) => ({
    phase: userDisconnect ? 'idle' : s.phase === 'ready' ? 'idle' : s.phase,
    sync: wasSyncing ? { ...s.sync!, state: 'failed', message: 'The unit disconnected. Sync again to continue.' } : s.sync,
  }));
  activeSession?.fail('The unit disconnected.');
}

async function teardown() {
  cancelEnrollment();
  // The disconnect listener goes first, so end a running sync here rather
  // than leave it to the stall timer.
  activeSession?.fail('The unit disconnected.');
  unsubscribers.forEach((u) => u());
  unsubscribers = [];
  disconnectSub?.remove();
  disconnectSub = null;
  const current = link.get().peripheralId;
  if (current) await ble.disconnect(current);
}

export async function disconnect() {
  userDisconnect = true;
  const gen = ++generation;
  const pending = inFlight?.promise;
  await teardown();
  link.set({ ...initial });
  // A connect that was under way may have set state while unwinding.
  if (pending) {
    await pending.catch(() => {});
    if (gen === generation) link.set({ ...initial });
  }
}

// ---------------------------------------------------------- confirmed writes

/**
 * Write a config characteristic and read it back (§5): a rejected write is
 * acknowledged anyway, and the device restores the value it is using.
 */
async function writeConfirmed<T>(chr: string, bytes: Uint8Array, decode: (d: Uint8Array) => T, matches: (v: T) => boolean) {
  await writeQ(chr, bytes);
  // Not sooner: until the unit's loop takes the write, ArduinoBLE reads back
  // the bytes just written, which would confirm a change the unit refuses.
  for (let attempt = 0; attempt < 4; attempt++) {
    await sleep(1000);
    const value = decode(await readQ(chr));
    if (matches(value)) return value;
  }
  throw new Error('The unit did not accept the change.');
}

export async function setMeasurementInterval(seconds: IntervalSeconds) {
  const config = link.get().config;
  if (!config) throw new Error('Config not loaded');
  const next = { ...config, intervalSeconds: seconds };
  const saved = await writeConfirmed(Chr.coreConfig, encodeCoreConfig(next), decodeCoreConfig, (c) => c.intervalSeconds === seconds);
  link.set({ config: saved });
}

export async function setAlwaysAvailable(on: boolean) {
  const config = link.get().config;
  if (!config) throw new Error('Config not loaded');
  const saved = await writeConfirmed(
    Chr.coreConfig,
    encodeCoreConfig({ ...config, bleAlwaysAvailable: on }),
    decodeCoreConfig,
    (c) => c.bleAlwaysAvailable === on,
  );
  link.set({ config: saved });
}

export async function setTemperatureUnit(unit: TemperatureUnit) {
  const config = link.get().config;
  if (!config) throw new Error('Config not loaded');
  const saved = await writeConfirmed(
    Chr.coreConfig,
    encodeCoreConfig({ ...config, temperatureUnit: unit }),
    decodeCoreConfig,
    (c) => c.temperatureUnit === unit,
  );
  link.set({ config: saved });
}

/** Every how many redraws the panel does a full, flashing refresh (1–1000). */
export async function setFullRefreshEvery(redraws: number) {
  const config = link.get().config;
  if (!config) throw new Error('Config not loaded');
  const saved = await writeConfirmed(
    Chr.coreConfig,
    encodeCoreConfig({ ...config, fullRefreshEvery: redraws }),
    decodeCoreConfig,
    (c) => c.fullRefreshEvery === redraws,
  );
  link.set({ config: saved });
}

export async function setUnitSleepWindow(window: UnitSleepWindow) {
  const saved = await writeConfirmed(Chr.sleepWindow, encodeSleepWindow(window), decodeSleepWindow, (w) => sameSleepWindow(w, window));
  link.set({ sleepWindow: saved });
}

export async function setScreen(screen: DisplayScreen) {
  await writeConfirmed(Chr.displayScreen, encodeDisplayScreen(screen), (d) => d[0] as DisplayScreen, (s) => s === screen);
  link.set((s) => ({ config: s.config ? { ...s.config, displayScreen: screen } : s.config }));
}

export async function setName(name: string) {
  const saved = await writeConfirmed(Chr.deviceName, encodeDeviceName(name), decodeDeviceName, (n) => n === name);
  link.set({ name: saved });
  const serial = link.get().serial;
  if (serial) await db.renameUnit(serial, saved);
}

export async function setOffsets(offsets: CalibrationOffsets) {
  const close = (a: number, b: number) => Math.abs(a - b) < 1e-3;
  const saved = await writeConfirmed(
    Chr.calibrationOffsets,
    encodeCalibrationOffsets(offsets),
    decodeCalibrationOffsets,
    (o) => close(o.noiseDb, offsets.noiseDb) && close(o.temperatureC, offsets.temperatureC) && close(o.humidityPct, offsets.humidityPct),
  );
  link.set({ offsets: saved });
}

// ----------------------------------------------------------------- commands

const statusListeners = new Set<(s: Status) => void>();

export function onStatus(listener: (s: Status) => void) {
  statusListeners.add(listener);
  return () => statusListeners.delete(listener);
}

/** Start a CO2 forced recalibration (§8); progress arrives in status. */
export async function startFrc(referencePpm: number) {
  await writeQ(Chr.calibrationControl, encodeFrc(referencePpm));
}

/** Factory reset (§6.11). The unit forgets every phone, so drop our key too. */
export async function factoryReset(eraseLog: boolean) {
  const serial = link.get().serial;
  await writeQ(Chr.deviceControl, encodeFactoryReset(eraseLog && can(Capability.logErase)));
  if (serial) await keys.deleteKey(serial);
}

// The unit takes the command from its main loop, which can stall for a few
// seconds behind a panel refresh.
const ERASE_CONFIRM_MS = 8_000;

/**
 * Erase the unit's whole log, provided it holds nothing newer than
 * upToSequence (capability logEraseCommand). Resolves once the unit reports
 * the erase under way; throws if it refused. Config, keys and offsets stay.
 */
export async function eraseLogUpTo(upToSequence: number) {
  if (!can(Capability.logEraseCommand)) throw new Error('This unit cannot erase its log on its own.');
  await writeQ(Chr.deviceControl, encodeLogErase(upToSequence));
  const deadline = Date.now() + ERASE_CONFIRM_MS;
  while (Date.now() < deadline) {
    await sleep(500);
    const status = decodeStatus(await readQ(Chr.status));
    if (status.logErasing) {
      link.set({ status });
      return;
    }
  }
  // Erasing ends in ~90 s, so a flag never seen means the unit said no:
  // most likely a record newer than upToSequence landed meanwhile.
  throw new Error('The unit did not erase its log.');
}

export async function reloadDiagnostics() {
  if (can(Capability.diagnostics)) link.set({ diagnostics: decodeDiagnostics(await readQ(Chr.diagnostics)) });
  if (can(Capability.calibrationState)) link.set({ calibration: decodeCalibrationState(await readQ(Chr.calibrationState)) });
}

// ------------------------------------------------------------- log download

interface ActiveSession {
  fail(message: string): void;
}
let activeSession: ActiveSession | null = null;

const STALL_MS = 15_000; // the device gives up after 10 s without delivery
const START_MS = 90_000; // a download rides the next measurement; longer during an FRC soak
const BATCH = 2000;
const MAX_FRUITLESS_ROUNDS = 3;

/**
 * Download every record the phone does not have yet (§7), in batches, and
 * save it. Resolves with the number of new records.
 */
let syncing: Promise<number> | null = null;

export function syncLog(): Promise<number> {
  // Claimed synchronously: a second caller joins the running sync.
  syncing ??= runSync().finally(() => {
    syncing = null;
  });
  return syncing;
}

async function runSync(): Promise<number> {
  const { serial, info } = link.get();
  if (!serial || !info || !hasCapability(info, Capability.logDownload)) throw new Error('This unit cannot send its log.');

  const unit = await db.getUnit(serial);
  let cursor = unit?.cursor ?? 0;
  const status = decodeStatus(await readQ(Chr.status));
  if (logWasReplaced(cursor, status.newestSequence)) cursor = 0;

  let total = 0;
  let fruitless = 0;
  // An upper bound: a log that wrapped holds fewer records than this.
  const expected = Math.max(0, status.newestSequence - cursor + 1);
  link.set({ sync: { state: 'waiting', received: 0, remaining: null, expected } });
  try {
    for (;;) {
      const { session, error } = await downloadBatch(cursor, total);
      // saveRecords drops records for a unit forgotten meanwhile.
      await db.saveRecords(serial, session.records);
      total += session.records.length;
      const next = session.nextCursor();
      await db.setCursor(serial, next, Date.now());
      if (session.lost || error) {
        // A dropped packet or a stalled stream: ask again from the first
        // record we lack. Give up after a few rounds that bring nothing.
        fruitless = session.records.length > 0 ? 0 : fruitless + 1;
        if (fruitless >= MAX_FRUITLESS_ROUNDS || link.get().phase !== 'ready') {
          throw error ?? new Error('The unit kept dropping data. Sync again to continue.');
        }
        trace('sync retry', `${cursor} -> ${next}`, session.lost ? 'packet lost' : errorText(error));
        cursor = next;
        continue;
      }
      const newest = link.get().status?.newestSequence ?? 0;
      if (!session.ended || session.records.length < BATCH || next > newest) break;
      cursor = next;
    }
    link.set({ sync: { state: 'done', received: total, remaining: 0, expected } });
    return total;
  } catch (e) {
    link.set((s) => ({ sync: { state: 'failed', received: total, remaining: s.sync?.remaining ?? null, expected, message: describeError(e) } }));
    throw e;
  }
}

/** Resolves with what arrived, and the error that cut it short if any. */
function downloadBatch(cursor: number, before: number): Promise<{ session: LogSession; error?: Error }> {
  const session = new LogSession(cursor);
  return new Promise<{ session: LogSession; error?: Error }>(async (resolve) => {
    let settled = false;
    let stallTimer: ReturnType<typeof setTimeout> | null = null;
    const cleanups: (() => void)[] = [];

    const finish = (err?: Error) => {
      if (settled) return;
      settled = true;
      if (stallTimer) clearTimeout(stallTimer);
      cleanups.forEach((c) => c());
      activeSession = null;
      // Keep what arrived intact: the cursor rules make a partial batch safe.
      resolve({ session, error: err });
    };
    const armStall = (ms: number) => {
      if (stallTimer) clearTimeout(stallTimer);
      stallTimer = setTimeout(() => finish(new Error('The unit stopped sending. Try again.')), ms);
    };

    activeSession = { fail: (m) => finish(new Error(m)) };
    try {
      // Subscribe before START, or the first notification stalls the session (§7.1).
      cleanups.push(
        await ble.subscribe(id(), QUIESCO_SERVICE, Chr.logSyncData, (packet) => {
          session.push(packet);
          link.set((s) => ({
            sync: { state: 'streaming', received: before + session.records.length, remaining: session.remaining, expected: s.sync?.expected ?? null },
          }));
          if (session.ended) finish();
          else if (session.lost) {
            // Stop the stream now rather than receive what we would ignore.
            void writeQ(Chr.logSyncControl, encodeLogSyncAbort()).catch(() => {});
            finish();
          }
          else armStall(STALL_MS);
        }),
      );
      cleanups.push(
        onStatus((s) => {
          // Streaming ended without END (abort, stall, flash error): keep what we have.
          if (!s.logDownloadActive && session.records.length > 0 && !session.ended) finish();
        }),
      );
      armStall(START_MS);
      await writeQ(Chr.logSyncControl, encodeLogSyncStart(cursor, BATCH, payload));
    } catch (e) {
      finish(e instanceof Error ? e : new Error(String(e)));
    }
  });
}

export async function abortSync() {
  activeSession?.fail('Sync cancelled.');
  await writeQ(Chr.logSyncControl, encodeLogSyncAbort()).catch(() => {});
}

// ------------------------------------------------------------------ errors

/** The message inside anything the BLE library rejects with. */
export function errorText(e: unknown): string {
  if (e instanceof Error) return e.message;
  if (typeof e === 'string') return e;
  if (e && typeof e === 'object' && 'message' in e && typeof e.message === 'string') return e.message;
  return String(e);
}

export function describeError(e: unknown): string {
  const text = errorText(e);
  if (/encrypt|authentic|insufficient/i.test(text)) {
    return 'The unit refused access. Plug it into USB power and add it again.';
  }
  if (/timed out/i.test(text)) return `${text}. Move closer to the unit and try again.`;
  if (/not connected|disconnect/i.test(text)) return 'The unit disconnected.';
  return text;
}
