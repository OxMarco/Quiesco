// Thin, typed wrapper over react-native-ble-manager: everything else in the
// app talks bytes (Uint8Array) and Quiesco UUIDs, never the library.

import { PermissionsAndroid, Platform } from 'react-native';
import BleManager, { BleState, type Peripheral, type StartOptions } from 'react-native-ble-manager';

import { redacted, trace } from '@/debug/trace';

import { MIN_ATT_PAYLOAD } from '@/protocol/log';
import { Chr, QUIESCO_SERVICE } from '@/protocol/uuids';

// Native BLE logging (discovered services, lookups, values) for the device
// console. Development builds only: it prints every value, keys included.
const VERBOSE_BLE = __DEV__;

let started: Promise<void> | null = null;

export function ensureStarted(): Promise<void> {
  // verboseLogging is read by the native module but missing from the types.
  started ??= BleManager.start({ showAlert: false, verboseLogging: VERBOSE_BLE } as StartOptions);
  return started;
}

export type RadioState = 'on' | 'off' | 'unauthorized' | 'unsupported' | 'unknown';

function toRadioState(state: BleState | string): RadioState {
  switch (state) {
    case BleState.On:
    case 'turning_on':
      return 'on';
    case BleState.Off:
    case 'turning_off':
      return 'off';
    case BleState.Unauthorized:
      return 'unauthorized';
    case BleState.Unsupported:
      return 'unsupported';
    default:
      return 'unknown';
  }
}

export async function radioState(): Promise<RadioState> {
  await ensureStarted();
  return toRadioState(await BleManager.checkState());
}

export function onRadioState(cb: (state: RadioState) => void) {
  return BleManager.onDidUpdateState((e) => cb(toRadioState(e.state)));
}

/** Android 12+ needs runtime scan/connect permissions; iOS asks on first use. */
export async function requestPermissions(): Promise<boolean> {
  if (Platform.OS !== 'android') return true;
  if (Platform.Version >= 31) {
    const result = await PermissionsAndroid.requestMultiple([
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
      PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    ]);
    return Object.values(result).every((r) => r === PermissionsAndroid.RESULTS.GRANTED);
  }
  const fine = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION);
  return fine === PermissionsAndroid.RESULTS.GRANTED;
}

export interface Advert {
  id: string;
  name: string;
  rssi: number;
}

function toAdvert(p: Peripheral): Advert {
  return { id: p.id, name: p.advertising?.localName ?? p.name ?? 'Quiesco', rssi: p.rssi };
}

/** Scan for units by service UUID (§2), never by name. Returns a stop function. */
export async function scan(onFound: (advert: Advert) => void, seconds = 15): Promise<() => void> {
  await ensureStarted();
  const sub = BleManager.onDiscoverPeripheral((p) => {
    trace('found', toAdvert(p));
    onFound(toAdvert(p));
  });
  try {
    await BleManager.scan({ serviceUUIDs: [QUIESCO_SERVICE], seconds, allowDuplicates: false });
  } catch (e) {
    sub.remove();
    throw e;
  }
  return () => {
    sub.remove();
    BleManager.stopScan().catch(() => {});
  };
}

/**
 * Connect and discover. Returns the short ids (e.g. "000C", "2A25") of every
 * characteristic the unit offers, for diagnosing a unit that lacks one.
 */
export async function connect(id: string): Promise<string[]> {
  await ensureStarted();
  await BleManager.connect(id);
  const info = await BleManager.retrieveServices(id);
  trace('services', (info.services ?? []).map((s) => shortId(s.uuid)));
  const offered = (info.characteristics ?? []).map((c) => `${shortId(c.service)}/${shortId(c.characteristic)}`);
  trace('characteristics', offered);
  return (info.characteristics ?? []).map((c) => shortId(c.characteristic));
}

function shortId(uuid: string): string {
  const u = uuid.toUpperCase();
  const quiesco = /^7A1E([0-9A-F]{4})-8E6F-4A7A-AE32-515549455343$/.exec(u);
  if (quiesco) return quiesco[1];
  const sig = /^0000([0-9A-F]{4})-0000-1000-8000-00805F9B34FB$/.exec(u);
  return sig ? sig[1] : u;
}

// Values that carry key material never reach a trace: the issued key read
// from 0011, and the proof written to 0010. Auth state reads (the challenge)
// are public.
const SECRET = { read: new Set([shortId(Chr.enrolKey)]), write: new Set([shortId(Chr.auth), shortId(Chr.enrolKey)]) };

/** What a trace may show of a value read from or written to `chr`. */
export function loggable(op: 'read' | 'write', chr: string, data: Uint8Array): Uint8Array | string {
  return SECRET[op].has(shortId(chr)) ? redacted(data) : data;
}

export async function disconnect(id: string): Promise<void> {
  await BleManager.disconnect(id).catch(() => {});
}

export function onDisconnect(cb: (id: string) => void) {
  return BleManager.onDisconnectPeripheral((e) => cb(e.peripheral));
}

/**
 * Negotiate the largest MTU and return the ATT payload (MTU − 3) to report
 * in a log START (§7.2). Android asks; iOS negotiates on its own and reports
 * its write limit, which is the same payload size.
 */
export async function negotiatePayload(id: string): Promise<number> {
  try {
    if (Platform.OS === 'android') {
      const mtu = await BleManager.requestMTU(id, 247);
      return Math.max(MIN_ATT_PAYLOAD, mtu - 3);
    }
    const max = await BleManager.getMaximumWriteValueLengthForWithoutResponse(id);
    return Math.max(MIN_ATT_PAYLOAD, max);
  } catch {
    return MIN_ATT_PAYLOAD;
  }
}

export async function read(id: string, service: string, chr: string): Promise<Uint8Array> {
  try {
    const data = Uint8Array.from(await BleManager.read(id, service, chr));
    trace('read', shortId(chr), loggable('read', chr, data));
    return data;
  } catch (e) {
    trace('read failed', shortId(chr), e);
    throw e;
  }
}

/** Write with response, never split: every Quiesco write fits one ATT write. */
export async function write(id: string, service: string, chr: string, data: Uint8Array): Promise<void> {
  try {
    await BleManager.write(id, service, chr, Array.from(data), Math.max(data.byteLength, 20));
    trace('write', shortId(chr), loggable('write', chr, data));
  } catch (e) {
    trace('write failed', shortId(chr), e);
    throw e;
  }
}

const sameUuid = (a: string, b: string) => {
  const norm = (u: string) => u.toUpperCase().replace(/^0000([0-9A-F]{4})-0000-1000-8000-00805F9B34FB$/, '$1');
  return norm(a) === norm(b);
};

/** Subscribe to notifications of one characteristic. Returns an unsubscribe. */
export async function subscribe(
  id: string,
  service: string,
  chr: string,
  cb: (data: Uint8Array) => void,
): Promise<() => void> {
  const sub = BleManager.onDidUpdateValueForCharacteristic((e) => {
    if (e.peripheral === id && sameUuid(e.characteristic, chr)) cb(Uint8Array.from(e.value));
  });
  try {
    await BleManager.startNotification(id, service, chr);
  } catch (e) {
    sub.remove();
    throw e;
  }
  return () => {
    sub.remove();
    BleManager.stopNotification(id, service, chr).catch(() => {});
  };
}
