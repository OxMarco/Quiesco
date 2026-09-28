// Encoders and decoders for every Quiesco characteristic (PROTOCOL.md §6).
// Mirrors firmware/src/protocol/BleCodec.cpp; the tests assert the golden
// vectors from PROTOCOL.md byte for byte.

import { ByteReader, ByteWriter } from './bytes';
import { hmacSha256 } from './sha256';

// Validity mask bits (§6.3), shared by reading, status and log records.
export const Valid = {
  temperature: 1 << 0,
  humidity: 1 << 1,
  pressure: 1 << 2,
  co2: 1 << 3,
  light: 1 << 4,
  noise: 1 << 5,
  battery: 1 << 6,
} as const;

export type MetricKey = 'co2' | 'temperature' | 'humidity' | 'noise' | 'light';

/** One set of values. A field is null when its validity bit is clear. */
export interface Measurement {
  temperatureC: number | null;
  humidityPct: number | null;
  pressurePa: number | null;
  co2Ppm: number | null;
  lux: number | null;
  noiseDb: number | null;
  batteryV: number | null;
  validMask: number;
}

function masked(mask: number, bit: number, value: number): number | null {
  return mask & bit ? value : null;
}

export class CodecError extends Error {}

function expectLength(what: string, data: Uint8Array, length: number): ByteReader {
  if (data.byteLength !== length) {
    throw new CodecError(`${what}: expected ${length} bytes, got ${data.byteLength}`);
  }
  return new ByteReader(data);
}

// ---------------------------------------------------------------- 6.1 interval

export const INTERVALS_S = [60, 300, 600, 1800] as const;
export type IntervalSeconds = (typeof INTERVALS_S)[number];

export function isValidInterval(seconds: number): seconds is IntervalSeconds {
  return (INTERVALS_S as readonly number[]).includes(seconds);
}

export function decodeInterval(data: Uint8Array): number {
  return expectLength('interval', data, 4).u32(0);
}

export function encodeInterval(seconds: IntervalSeconds): Uint8Array {
  return new ByteWriter(4).u32(0, seconds).bytes;
}

// ------------------------------------------------------------- 6.2 core config

export enum DisplayScreen {
  Face = 0,
  Ledger = 1,
  Bento = 2,
}

/** How the panel writes temperatures (capability bit 12; reads 0 before it). */
export enum TemperatureUnit {
  Celsius = 0,
  Fahrenheit = 1,
}

export interface CoreConfig {
  version: number;
  intervalSeconds: number;
  fullRefreshEvery: number;
  bleAlwaysAvailable: boolean;
  displayScreen: DisplayScreen;
  temperatureUnit: TemperatureUnit;
}

export function decodeCoreConfig(data: Uint8Array): CoreConfig {
  const r = expectLength('core config', data, 16);
  return {
    version: r.u32(0),
    intervalSeconds: r.u32(4),
    fullRefreshEvery: r.u32(8),
    bleAlwaysAvailable: r.u8(12) !== 0,
    displayScreen: r.u8(13) as DisplayScreen,
    temperatureUnit: r.u8(14) as TemperatureUnit,
  };
}

/** Write back a modified copy of the last value read (read, modify, write). */
export function encodeCoreConfig(config: CoreConfig): Uint8Array {
  if (!isValidInterval(config.intervalSeconds)) {
    throw new CodecError(`interval ${config.intervalSeconds} s is not allowed`);
  }
  if (config.fullRefreshEvery < 1 || config.fullRefreshEvery > 1000) {
    throw new CodecError('full refresh must be 1–1000 redraws');
  }
  return new ByteWriter(16)
    .u32(0, config.version)
    .u32(4, config.intervalSeconds)
    .u32(8, config.fullRefreshEvery)
    .u8(12, config.bleAlwaysAvailable ? 1 : 0)
    .u8(13, config.displayScreen)
    .u8(14, config.temperatureUnit).bytes;
}

// ---------------------------------------------------------- 6.3 latest reading

export function decodeReading(data: Uint8Array): Measurement {
  const r = expectLength('reading', data, 20);
  const m = r.u8(18);
  return {
    temperatureC: masked(m, Valid.temperature, r.i16(0) / 100),
    humidityPct: masked(m, Valid.humidity, r.u16(2) / 100),
    pressurePa: masked(m, Valid.pressure, r.u32(4)),
    co2Ppm: masked(m, Valid.co2, r.u16(8)),
    lux: masked(m, Valid.light, r.u32(10) / 10),
    noiseDb: masked(m, Valid.noise, r.i16(14) / 10),
    batteryV: masked(m, Valid.battery, r.u16(16) / 1000),
    validMask: m,
  };
}

// ------------------------------------------------------------------ 6.4 status

/** Device order for the present, timed-out and failure fields (§6.4). */
export const STATUS_DEVICES = [
  'Climate sensor',
  'Light sensor',
  'CO₂ sensor',
  'Battery monitor',
  'Microphone',
  'Display',
  'Flash',
  'Bluetooth',
] as const;

export enum FrcState {
  Idle = 0,
  Pending = 1,
  Soaking = 2,
  Done = 3,
  Failed = 4,
}

export interface Status {
  validMask: number;
  presentMask: number;
  timedOutMask: number;
  failures: number[];
  newestSequence: number;
  clockSynced: boolean;
  logDownloadActive: boolean;
  logErasing: boolean;
  /** Charger reports charging (capability chargingState). */
  charging: boolean;
  frcState: FrcState;
  frcCorrectionPpm: number;
}

export function decodeStatus(data: Uint8Array): Status {
  const r = expectLength('status', data, 20);
  const flags = r.u8(16);
  return {
    validMask: r.u16(0),
    presentMask: r.u8(2),
    timedOutMask: r.u8(3),
    failures: Array.from({ length: 8 }, (_, i) => r.u8(4 + i)),
    newestSequence: r.u32(12),
    clockSynced: (flags & 1) !== 0,
    logDownloadActive: (flags & 2) !== 0,
    logErasing: (flags & 4) !== 0,
    charging: (flags & 8) !== 0,
    frcState: r.u8(17) as FrcState,
    frcCorrectionPpm: r.i16(18),
  };
}

// -------------------------------------------------------------- 6.5 epoch time

export function encodeEpoch(epochMs: number): Uint8Array {
  if (!(epochMs > 0) || epochMs / 1000 >= 2 ** 32) {
    throw new CodecError('epoch must be non-zero and below 2^32 s');
  }
  return new ByteWriter(8).u64(0, Math.round(epochMs)).bytes;
}

export function decodeEpoch(data: Uint8Array): number {
  return expectLength('epoch', data, 8).u64(0);
}

// ---------------------------------------------------------- 6.6 display screen

export function decodeDisplayScreen(data: Uint8Array): DisplayScreen {
  return expectLength('display screen', data, 1).u8(0) as DisplayScreen;
}

export function encodeDisplayScreen(screen: DisplayScreen): Uint8Array {
  return new ByteWriter(1).u8(0, screen).bytes;
}

// ----------------------------------------------------- 6.7 calibration offsets

export interface CalibrationOffsets {
  noiseDb: number;
  temperatureC: number;
  humidityPct: number;
}

export const OFFSET_LIMITS: CalibrationOffsets = { noiseDb: 24, temperatureC: 8, humidityPct: 20 };

export function decodeCalibrationOffsets(data: Uint8Array): CalibrationOffsets {
  const r = expectLength('calibration offsets', data, 12);
  return { noiseDb: r.f32(0), temperatureC: r.f32(4), humidityPct: r.f32(8) };
}

export function encodeCalibrationOffsets(o: CalibrationOffsets): Uint8Array {
  for (const key of ['noiseDb', 'temperatureC', 'humidityPct'] as const) {
    if (!Number.isFinite(o[key]) || Math.abs(o[key]) > OFFSET_LIMITS[key]) {
      throw new CodecError(`${key} offset out of range ±${OFFSET_LIMITS[key]}`);
    }
  }
  return new ByteWriter(12).f32(0, o.noiseDb).f32(4, o.temperatureC).f32(8, o.humidityPct).bytes;
}

// ------------------------------------------------------------- 6.8 device name

export function utf8Encode(text: string): Uint8Array {
  const out: number[] = [];
  for (const ch of text) {
    const cp = ch.codePointAt(0)!;
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 63));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
  }
  return Uint8Array.from(out);
}

export function utf8Decode(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; ) {
    const b = bytes[i];
    let cp: number;
    let n: number;
    if (b < 0x80) [cp, n] = [b, 1];
    else if (b >= 0xf0) [cp, n] = [b & 7, 4];
    else if (b >= 0xe0) [cp, n] = [b & 15, 3];
    else [cp, n] = [b & 31, 2];
    for (let k = 1; k < n; k++) cp = (cp << 6) | ((bytes[i + k] ?? 0x80) & 63);
    out += String.fromCodePoint(cp);
    i += n;
  }
  return out;
}

export const DEVICE_NAME_MAX_BYTES = 16;

/** Why a name would be rejected by the device, or null when it is valid. */
export function deviceNameProblem(name: string): string | null {
  const bytes = utf8Encode(name);
  if (bytes.length === 0) return 'Enter a name.';
  if (bytes.length > DEVICE_NAME_MAX_BYTES) return 'Use at most 16 bytes (fewer with accents or emoji).';
  if (bytes.some((b) => b < 0x20 || b === 0x7f)) return 'Remove line breaks and control characters.';
  return null;
}

export function encodeDeviceName(name: string): Uint8Array {
  const problem = deviceNameProblem(name);
  if (problem) throw new CodecError(problem);
  return utf8Encode(name);
}

export function decodeDeviceName(data: Uint8Array): string {
  return utf8Decode(data);
}

// ---------------------------------------------------------- 6.9 log sync control

export function encodeLogSyncStart(firstSequence: number, maxRecords: number, attPayload: number): Uint8Array {
  return new ByteWriter(10).u8(0, 1).u32(2, firstSequence).u16(6, maxRecords).u16(8, attPayload).bytes;
}

export function encodeLogSyncAbort(): Uint8Array {
  return new ByteWriter(10).u8(0, 2).bytes;
}

// ------------------------------------------------------ 6.10 calibration control

export function encodeFrc(referencePpm: number): Uint8Array {
  if (!Number.isInteger(referencePpm) || referencePpm < 400 || referencePpm > 2000) {
    throw new CodecError('reference must be 400–2000 ppm');
  }
  return new ByteWriter(4).u8(0, 1).u16(2, referencePpm).bytes;
}

// ----------------------------------------------------------- 6.11 device control

export const FACTORY_RESET_CONFIRM = 0xfac7;

export function encodeFactoryReset(eraseLog: boolean): Uint8Array {
  return new ByteWriter(4).u8(0, 1).u8(1, eraseLog ? 1 : 0).u16(2, FACTORY_RESET_CONFIRM).bytes;
}

// -------------------------------------------------------------- 6.12 device info

export const Capability = {
  displayScreen: 1 << 0,
  calibrationOffsets: 1 << 1,
  frc: 1 << 2,
  rename: 1 << 3,
  logDownload: 1 << 4,
  factoryReset: 1 << 5,
  logErase: 1 << 6,
  bootCounter: 1 << 7,
  calibrationState: 1 << 8,
  diagnostics: 1 << 9,
  appAuth: 1 << 10,
  chargingState: 1 << 11,
  temperatureUnit: 1 << 12,
} as const;

export interface DeviceInfo {
  protocolVersion: number;
  capabilities: number;
  firmware: string;
  debugBuild: boolean;
  pairingOpen: boolean;
  bonded: boolean;
  /** Bench build without a cell: show no battery at all. */
  noBattery: boolean;
  scd41Serial: number;
  bootCounter: number;
}

export function decodeDeviceInfo(data: Uint8Array): DeviceInfo {
  const r = expectLength('device info', data, 20);
  const flags = r.u8(9);
  return {
    protocolVersion: r.u16(0),
    capabilities: r.u32(2),
    firmware: `${r.u8(6)}.${r.u8(7)}.${r.u8(8)}`,
    debugBuild: (flags & 1) !== 0,
    pairingOpen: (flags & 2) !== 0,
    bonded: (flags & 4) !== 0,
    noBattery: (flags & 8) !== 0,
    scd41Serial: r.u48(10),
    bootCounter: r.u32(16),
  };
}

export function hasCapability(info: DeviceInfo, bit: number): boolean {
  return (info.capabilities & bit) !== 0;
}

// -------------------------------------------------------- 6.13 calibration state

export interface CalibrationState {
  lastFrcEpochS: number;
  lastFrcReferencePpm: number;
  lastFrcCorrectionPpm: number;
  ascOff: boolean;
}

export function decodeCalibrationState(data: Uint8Array): CalibrationState {
  const r = expectLength('calibration state', data, 12);
  return {
    lastFrcEpochS: r.u32(0),
    lastFrcReferencePpm: r.u16(4),
    lastFrcCorrectionPpm: r.i16(6),
    ascOff: (r.u8(8) & 1) !== 0,
  };
}

// -------------------------------------------------------------- 6.14 diagnostics

export interface Diagnostics {
  resetReason: number;
  uptimeSeconds: number;
  i2cBusStuck: boolean;
}

export function decodeDiagnostics(data: Uint8Array): Diagnostics {
  const r = expectLength('diagnostics', data, 12);
  return { resetReason: r.u32(0), uptimeSeconds: r.u32(4), i2cBusStuck: (r.u8(8) & 1) !== 0 };
}

const RESET_CAUSES: [number, string][] = [
  [0, 'reset pin'],
  [1, 'watchdog'],
  [2, 'software reset'],
  [3, 'CPU lockup'],
  [16, 'wake from System OFF'],
  [20, 'USB power'],
];

export function describeResetReason(bits: number): string {
  if (bits === 0) return 'power-on or brownout';
  const names = RESET_CAUSES.filter(([bit]) => bits & (1 << bit)).map(([, name]) => name);
  return names.length ? names.join(', ') : `0x${bits.toString(16)}`;
}

// ------------------------------------------------ 6.15 / 6.16 authentication

export interface AuthState {
  authenticated: boolean;
  enrolmentOpen: boolean;
  enrolled: boolean;
  challenge: Uint8Array;
}

export function decodeAuthState(data: Uint8Array): AuthState {
  const r = expectLength('auth', data, 20);
  const flags = r.u8(0);
  return {
    authenticated: (flags & 1) !== 0,
    enrolmentOpen: (flags & 2) !== 0,
    enrolled: (flags & 4) !== 0,
    challenge: data.slice(2, 18),
  };
}

export interface EnrolledKey {
  keyId: number;
  key: Uint8Array;
}

/** Null while the enrolment key is blank (refused, expired, or not ready). */
export function decodeEnrolKey(data: Uint8Array): EnrolledKey | null {
  const r = expectLength('enrolment key', data, 20);
  const keyId = r.u16(0);
  return keyId === 0 ? null : { keyId, key: data.slice(4, 20) };
}

export function encodeEnrol(): Uint8Array {
  return Uint8Array.of(1, 0);
}

const AUTH_LABEL = Uint8Array.from('QAUTH1', (c) => c.charCodeAt(0));

/** First 16 bytes of HMAC-SHA-256(key, "QAUTH1" || challenge || keyId LE). */
export function authProof(key: Uint8Array, challenge: Uint8Array, keyId: number): Uint8Array {
  return hmacSha256(key, AUTH_LABEL, challenge, Uint8Array.of(keyId & 0xff, keyId >> 8)).slice(0, 16);
}

const SETUP_LABEL = Uint8Array.from('QSETUP1', (c) => c.charCodeAt(0));

/**
 * Protocol v6: the key proved during setup, from the six digits on the unit's
 * screen. First 16 bytes of HMAC-SHA-256(code as ASCII, "QSETUP1" || keyId LE).
 */
export function setupKey(code: string, keyId: number): Uint8Array {
  if (!/^\d{6}$/.test(code)) throw new CodecError('setup code must be six digits');
  const digits = Uint8Array.from(code, (c) => c.charCodeAt(0));
  return hmacSha256(digits, SETUP_LABEL, Uint8Array.of(keyId & 0xff, keyId >> 8)).slice(0, 16);
}

export function encodeProve(keyId: number, key: Uint8Array, challenge: Uint8Array): Uint8Array {
  const out = new ByteWriter(20).u8(0, 2).u16(2, keyId).bytes;
  out.set(authProof(key, challenge, keyId), 4);
  return out;
}
