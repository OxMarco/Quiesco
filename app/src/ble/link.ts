// One live connection to a Quiesco unit: the connect sequence (PROTOCOL.md §2),
// pairing (§3), confirmed writes (§5) and log download (§7).

import { Platform } from 'react-native';

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
  EnrolledKey,
  encodeEpoch,
  encodeFactoryReset,
  encodeFrc,
  encodeLogSyncAbort,
  encodeLogSyncStart,
  hasCapability,
  IntervalSeconds,
  Measurement,
  Status,
  TemperatureUnit,
  utf8Decode,
} from '@/protocol/codec';
import { LogSession, logWasReplaced } from '@/protocol/log';
import { Chr, Dis, DIS_SERVICE, KNOWN_PROTOCOL_VERSIONS, QUIESCO_SERVICE } from '@/protocol/uuids';

import { trace } from '@/debug/trace';

import * as keys from './keys';
import { cancelEnrollment, requestSetupKey } from './enrollment';

import { Store } from './store';
import * as ble from './transport';

export type Phase =
  | 'idle'
  | 'connecting'
  | 'pairing' // the phone OS is showing its pairing prompt
  | 'needsUsb' // not bonded and the unit is not on USB power (§3)
  | 'incompatible'
  | 'ready'
  | 'error';

export interface SyncProgress {
  state: 'waiting' | 'streaming' | 'saving' | 'done' | 'failed';
  received: number;
  remaining: number | null;
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

/**
 * Connect and run the recommended sequence (§2). Resolves once the link is
 * ready, or leaves the phase at needsUsb / incompatible / error.
 */
export async function connect(peripheralId: string): Promise<void> {
  await teardown();
  userDisconnect = false;
  link.set({ ...initial, phase: 'connecting', peripheralId });
  trace('connect', peripheralId);
  try {
    const offered = await withTimeout(ble.connect(peripheralId), 15_000, 'Connecting');
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
    if (!KNOWN_PROTOCOL_VERSIONS.includes(info.protocolVersion)) {
      link.set({ phase: 'incompatible' });
      await ble.disconnect(peripheralId);
      return;
    }

    // 4: access. Protocol v4 units authenticate at the app layer; older ones
    // rely on link-layer pairing, which the unit accepts only on USB power.
    if (hasCapability(info, Capability.appAuth)) {
      await authenticate(serial);
    } else {
      await secureLink(info);
    }
    if (link.get().phase === 'needsUsb') return;

    // 5–7: clock, live values, config.
    await writeQ(Chr.epoch, encodeEpoch(Date.now()));
    await subscribeLive();
    await refreshAll();

    const name = link.get().name ?? `Quiesco ${serial.slice(-4)}`;
    await db.upsertUnit({ serial, peripheralId, name, firmware });
    link.set({ phase: 'ready' });
    trace('ready');
  } catch (e) {
    trace('connect failed', e);
    if (link.get().phase !== 'needsUsb') {
      link.set({ phase: 'error', error: describeError(e) });
    }
    await ble.disconnect(peripheralId);
  }
}

async function secureLink(info: DeviceInfo) {
  const peripheralId = id();
  let bondedOnPhone = false;
  if (Platform.OS === 'android') {
    bondedOnPhone = (await ble.bondedIds()).includes(peripheralId);
  }
  if (!bondedOnPhone && !info.pairingOpen && !info.bonded) {
    // Nothing on either side and pairing is closed: ask for USB first rather
    // than let the OS prompt fail.
    link.set({ phase: 'needsUsb' });
    await ble.disconnect(peripheralId);
    return;
  }
  link.set({ phase: 'pairing' });
  try {
    if (Platform.OS === 'android' && !bondedOnPhone) {
      await withTimeout(ble.createBond(peripheralId), 90_000, 'Pairing');
    }
    link.set({ status: await readStatusOncePaired(peripheralId) });
    link.set({ phase: 'connecting' });
  } catch (e) {
    if (!info.pairingOpen) {
      link.set({ phase: 'needsUsb' });
      await ble.disconnect(peripheralId);
      return;
    }
    throw e;
  }
}

// ---------------------------------------------- app-layer auth (protocol v4)

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
  for (let round = 0; round < 2; round++) {
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
      // Do not retain mistyped or unconfirmed setup keys. From v6 the setup
      // key only admits setup: the unit then hands over the phone's own key.
      if (isNewKey) {
        if ((link.get().info?.protocolVersion ?? 0) >= 6) enrolled = await readIssuedKey(enrolled.keyId);
        await keys.saveKey(serial, enrolled);
      }
      trace('authenticated', { keyId: enrolled.keyId });
      link.set({ phase: 'connecting' });
      return;
    }
    if (isNewKey) throw new Error('The code was rejected or expired. Reconnect and enter the new code shown on the unit.');
    // The unit no longer knows this key: factory reset, or evicted by a fifth
    // phone. Forget it and enrol again if the unit is on USB.
    trace('key rejected', { keyId: enrolled.keyId });
    await keys.deleteKey(serial);
    enrolled = null;
  }
  link.set({ phase: 'needsUsb' });
  await ble.disconnect(id());
}

async function enrol(): Promise<EnrolledKey> {
  const info = link.get().info!;
  await writeQ(Chr.auth, encodeEnrol());
  const deadline = Date.now() + AUTH_WAIT_MS;
  while (Date.now() < deadline) {
    await sleep(AUTH_POLL_MS);
    const bytes = await readQ(Chr.enrolKey);
    if ((link.get().info?.protocolVersion ?? 0) >= 5) {
      if (bytes.length !== 20 || bytes.slice(2).some((b) => b !== 0)) {
        throw new Error('Invalid setup response from the unit.');
      }
      const keyId = bytes[0] | (bytes[1] << 8);
      if (keyId !== 0) return { keyId, key: await requestSetupKey(keyId, info.protocolVersion >= 6 ? 'code' : 'key') };
    } else {
      const enrolled = decodeEnrolKey(bytes);
      if (enrolled) return enrolled;
    }
  }
  throw new Error('The unit did not issue a key. Keep it on USB power and try again.');
}

/** v6: the phone key the unit places in 0011 once the setup code is proved. */
async function readIssuedKey(keyId: number): Promise<EnrolledKey> {
  const deadline = Date.now() + AUTH_WAIT_MS;
  while (Date.now() < deadline) {
    const issued = decodeEnrolKey(await readQ(Chr.enrolKey));
    if (issued && issued.keyId === keyId && issued.key.some((b) => b !== 0)) return issued;
    await sleep(AUTH_POLL_MS);
  }
  throw new Error('The unit accepted the code but did not hand over a key. Reconnect and try again.');
}

/** True when the unit accepted the proof; false when it rejected the key. */
async function prove(enrolled: EnrolledKey, challenge: Uint8Array): Promise<boolean> {
  if (challenge.every((b) => b === 0)) throw new Error('The unit has no challenge ready. Try again.');
  await writeQ(Chr.auth, encodeProve(enrolled.keyId, enrolled.key, challenge));
  const deadline = Date.now() + AUTH_WAIT_MS;
  while (Date.now() < deadline) {
    await sleep(AUTH_POLL_MS);
    const state = decodeAuthState(await readQ(Chr.auth));
    if (state.authenticated) return true;
    // A new challenge without the flag: the proof was checked and refused.
    if (state.challenge.some((b, i) => b !== challenge[i])) return false;
  }
  throw new Error('The unit did not answer the sign-in. Try again.');
}

const PAIRING_MS = 90_000;
const PAIRING_RETRY_MS = 1_500;

/**
 * Read status, which needs encryption, waiting for the OS to pair. On iOS the
 * read that starts pairing can fail with Insufficient Encryption while the
 * pairing prompt is still on screen; disconnecting then would abort the pairing
 * the user is being asked to confirm. So stay connected and retry until the
 * link is encrypted, the unit disconnects, or the pairing window passes.
 */
async function readStatusOncePaired(peripheralId: string): Promise<Status> {
  const deadline = Date.now() + PAIRING_MS;
  for (let attempt = 1; ; attempt++) {
    try {
      return decodeStatus(await withTimeout(ble.read(peripheralId, QUIESCO_SERVICE, Chr.status), PAIRING_MS, 'Pairing'));
    } catch (e) {
      const text = errorText(e);
      const waitingForPairing = /encrypt|authentic|insufficient/i.test(text);
      trace('pairing wait', { attempt, error: text });
      if (!waitingForPairing || Date.now() > deadline || link.get().peripheralId !== peripheralId) throw e;
      await sleep(PAIRING_RETRY_MS);
    }
  }
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
  link.set(patch);
}

function onDropped() {
  cancelEnrollment('The unit disconnected. Reconnect to get a new setup code.');
  const wasSyncing = link.get().sync?.state === 'waiting' || link.get().sync?.state === 'streaming';
  unsubscribers = [];
  link.set((s) => ({
    phase: userDisconnect ? 'idle' : s.phase === 'ready' ? 'idle' : s.phase,
    sync: wasSyncing ? { ...s.sync!, state: 'failed', message: 'The unit disconnected. Sync again to continue.' } : s.sync,
  }));
  activeSession?.fail('The unit disconnected.');
}

async function teardown() {
  cancelEnrollment();
  unsubscribers.forEach((u) => u());
  unsubscribers = [];
  disconnectSub?.remove();
  disconnectSub = null;
  const current = link.get().peripheralId;
  if (current) await ble.disconnect(current);
}

export async function disconnect() {
  userDisconnect = true;
  await teardown();
  link.set({ ...initial });
}

// ---------------------------------------------------------- confirmed writes

/**
 * Write a config characteristic and read it back (§5): a rejected write is
 * acknowledged anyway, and the device restores the value it is using.
 */
async function writeConfirmed<T>(chr: string, bytes: Uint8Array, decode: (d: Uint8Array) => T, matches: (v: T) => boolean) {
  await writeQ(chr, bytes);
  for (let attempt = 0; attempt < 4; attempt++) {
    await sleep(attempt === 0 ? 150 : 1000);
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

export async function factoryReset(eraseLog: boolean) {
  await writeQ(Chr.deviceControl, encodeFactoryReset(eraseLog && can(Capability.logErase)));
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

/**
 * Download every record the phone does not have yet (§7), in batches, and
 * save it. Resolves with the number of new records.
 */
export async function syncLog(): Promise<number> {
  const { serial, info } = link.get();
  if (!serial || !info || !hasCapability(info, Capability.logDownload)) throw new Error('This unit cannot send its log.');
  if (activeSession) throw new Error('A sync is already running.');

  const unit = await db.getUnit(serial);
  let cursor = unit?.cursor ?? 0;
  const status = decodeStatus(await readQ(Chr.status));
  if (logWasReplaced(cursor, status.newestSequence)) cursor = 0;

  let total = 0;
  link.set({ sync: { state: 'waiting', received: 0, remaining: null } });
  try {
    for (;;) {
      const session = await downloadBatch(cursor, total);
      await db.saveRecords(serial, session.records);
      total += session.records.length;
      const next = session.nextCursor();
      await db.setCursor(serial, next, Date.now());
      const newest = link.get().status?.newestSequence ?? 0;
      if (!session.ended || session.records.length < BATCH || next > newest) break;
      cursor = next;
    }
    link.set({ sync: { state: 'done', received: total, remaining: 0 } });
    return total;
  } catch (e) {
    link.set((s) => ({ sync: { state: 'failed', received: total, remaining: s.sync?.remaining ?? null, message: describeError(e) } }));
    throw e;
  }
}

function downloadBatch(cursor: number, before: number): Promise<LogSession> {
  const session = new LogSession(cursor);
  return new Promise<LogSession>(async (resolve, reject) => {
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
      if (err && session.records.length === 0) reject(err);
      else resolve(session);
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
          link.set({ sync: { state: 'streaming', received: before + session.records.length, remaining: session.remaining } });
          if (session.ended) finish();
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
