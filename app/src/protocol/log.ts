// Log download (PROTOCOL.md §7): packet parsing, fragment reassembly, record
// decoding and the phone-side cursor rules.

import { ByteReader } from './bytes';
import { Measurement, Valid } from './codec';

export const LOG_HEADER_BYTES = 12;
export const WIRE_RECORD_BYTES = 52;
export const MIN_ATT_PAYLOAD = 20;
export const MAX_LOG_PACKET_BYTES = 180;

/** CRC-8/SMBUS: polynomial 0x07, init 0, no reflection, no final XOR. */
export function crc8(data: Uint8Array): number {
  let crc = 0;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
    }
  }
  return crc;
}

export enum PacketType {
  Data = 1,
  End = 2,
  Fragment = 3,
}

export interface PacketHeader {
  type: PacketType;
  counter: number;
  /** DATA: first record's sequence. FRAGMENT: the record's. END: next to request. */
  sequence: number;
  recordCount: number;
  fragmentIndex: number;
  remaining: number;
}

/** Returns null for a packet that is too short or fails its CRC (discard it). */
export function parsePacketHeader(packet: Uint8Array): PacketHeader | null {
  if (packet.byteLength < LOG_HEADER_BYTES) return null;
  const copy = packet.slice();
  const expected = copy[11];
  copy[11] = 0;
  if (crc8(copy) !== expected) return null;
  const r = new ByteReader(packet);
  const type = r.u8(0);
  if (type !== PacketType.Data && type !== PacketType.End && type !== PacketType.Fragment) return null;
  return {
    type,
    counter: r.u8(1),
    sequence: r.u32(2),
    recordCount: r.u8(6),
    fragmentIndex: r.u8(7),
    remaining: r.u16(8),
  };
}

export interface LogRecord extends Measurement {
  sequence: number;
  /** Unix seconds, or null when the clock was not synced when it was taken. */
  epochS: number | null;
  msSinceBoot: number;
  /** 0 = unknown. */
  bootCounter: number;
}

export function decodeRecord(wire: Uint8Array): LogRecord {
  if (wire.byteLength !== WIRE_RECORD_BYTES) throw new Error('record must be 52 bytes');
  const r = new ByteReader(wire);
  const m = r.u16(4);
  const timeValid = (r.u16(6) & 1) !== 0;
  const f = (bit: number, offset: number) => (m & bit ? r.f32(offset) : null);
  return {
    sequence: r.u32(0),
    validMask: m,
    epochS: timeValid ? r.u32(8) : null,
    msSinceBoot: r.u64(12),
    temperatureC: f(Valid.temperature, 20),
    humidityPct: f(Valid.humidity, 24),
    pressurePa: f(Valid.pressure, 28),
    co2Ppm: f(Valid.co2, 32),
    lux: f(Valid.light, 36),
    noiseDb: f(Valid.noise, 40),
    batteryV: f(Valid.battery, 44),
    bootCounter: r.u32(48),
  };
}

/** Clamp the reported payload the way the device does (§7.2). */
export function clampAttPayload(reported: number): number {
  return Math.min(MAX_LOG_PACKET_BYTES, Math.max(MIN_ATT_PAYLOAD, reported));
}

/**
 * Consumes log-data notifications for one download session. Feed every packet
 * to `push`; read `records`, `ended` and `nextCursor()` as it goes.
 */
export class LogSession {
  readonly records: LogRecord[] = [];
  ended = false;
  /** Packets dropped for a bad CRC, a bad layout or a broken fragment chain. */
  discarded = 0;
  /** Latest progress hint: records still to come after the last packet. */
  remaining: number | null = null;
  /**
   * A packet went missing in transit (its counter skipped, §7.2): everything
   * after it is ignored, so nextCursor() points at the first record we lack
   * and asking again from there loses nothing.
   */
  lost = false;

  private endNext: number | null = null;
  private highest = -1;
  private nextCounter = 0;
  private fragSequence = -1;
  private fragNextIndex = 0;
  private fragChunks: Uint8Array[] = [];

  constructor(readonly requestedFrom: number) {}

  push(packet: Uint8Array): void {
    if (this.ended || this.lost) return;
    const header = parsePacketHeader(packet);
    if (!header) {
      this.discarded++;
      this.dropFragments();
      return;
    }
    // DATA and FRAGMENT count up from 0; END carries the next count. A skip
    // means the stack dropped a packet (or one failed its CRC above).
    if (header.counter !== this.nextCounter) {
      this.lost = true;
      this.dropFragments();
      return;
    }
    if (header.type !== PacketType.End) this.nextCounter = (this.nextCounter + 1) & 0xff;
    switch (header.type) {
      case PacketType.Data:
        this.onData(header, packet);
        break;
      case PacketType.Fragment:
        this.onFragment(header, packet);
        break;
      case PacketType.End:
        this.dropFragments();
        this.endNext = header.sequence;
        this.ended = true;
        this.remaining = 0;
        break;
    }
  }

  /**
   * The cursor to save (§7.4): END's next sequence, otherwise one past the
   * highest record received intact, otherwise where we started.
   */
  nextCursor(): number {
    if (this.endNext !== null) return this.endNext;
    return this.highest >= 0 ? this.highest + 1 : this.requestedFrom;
  }

  /** True when the device no longer holds records we asked for (§7.4 gaps). */
  get hasGap(): boolean {
    return this.records.length > 0 && this.records[0].sequence > this.requestedFrom;
  }

  private onData(header: PacketHeader, packet: Uint8Array) {
    this.dropFragments();
    const expected = LOG_HEADER_BYTES + header.recordCount * WIRE_RECORD_BYTES;
    if (header.recordCount < 1 || packet.byteLength < expected) {
      this.discarded++;
      return;
    }
    for (let i = 0; i < header.recordCount; i++) {
      const start = LOG_HEADER_BYTES + i * WIRE_RECORD_BYTES;
      this.accept(decodeRecord(packet.slice(start, start + WIRE_RECORD_BYTES)));
    }
    this.remaining = header.remaining;
  }

  private onFragment(header: PacketHeader, packet: Uint8Array) {
    if (header.fragmentIndex === 0) {
      this.dropFragments();
      this.fragSequence = header.sequence;
    } else if (header.sequence !== this.fragSequence || header.fragmentIndex !== this.fragNextIndex) {
      // A fragment went missing: the record cannot be rebuilt (§7.2).
      this.dropFragments();
      this.discarded++;
      return;
    }
    this.fragChunks.push(packet.slice(LOG_HEADER_BYTES));
    this.fragNextIndex = header.fragmentIndex + 1;
    this.remaining = header.remaining;

    const size = this.fragChunks.reduce((n, c) => n + c.byteLength, 0);
    if (size >= WIRE_RECORD_BYTES) {
      const wire = new Uint8Array(size);
      let offset = 0;
      for (const chunk of this.fragChunks) {
        wire.set(chunk, offset);
        offset += chunk.byteLength;
      }
      this.dropFragments();
      if (size === WIRE_RECORD_BYTES) this.accept(decodeRecord(wire));
      else this.discarded++;
    }
  }

  private accept(record: LogRecord) {
    // Sequences only increase; anything else is a duplicate or corrupt.
    if (record.sequence <= this.highest) return;
    this.highest = record.sequence;
    this.records.push(record);
  }

  private dropFragments() {
    this.fragSequence = -1;
    this.fragNextIndex = 0;
    this.fragChunks = [];
  }
}

/**
 * §7.4: the log was replaced when the device's newest sequence falls behind
 * our cursor. A newest of 0 is no evidence: firmware reports 0 until it mounts
 * its log, which it does lazily after boot. Waiting for a later status loses
 * nothing, while resetting would download the whole log again.
 */
export function logWasReplaced(cursor: number, newestSequence: number): boolean {
  return cursor > 0 && newestSequence > 0 && newestSequence < cursor - 1;
}

/**
 * Date records that lack wall-clock time from a synced record of the same boot
 * (§7.4). Returns a map from sequence to Unix seconds for the records it could
 * date; records from an unknown boot (counter 0) are never matched.
 */
export function inferEpochs(
  undated: Pick<LogRecord, 'sequence' | 'bootCounter' | 'msSinceBoot'>[],
  anchors: Pick<LogRecord, 'bootCounter' | 'msSinceBoot' | 'epochS'>[],
): Map<number, number> {
  const byBoot = new Map<number, { msSinceBoot: number; epochS: number }>();
  for (const a of anchors) {
    if (a.bootCounter !== 0 && a.epochS !== null && !byBoot.has(a.bootCounter)) {
      byBoot.set(a.bootCounter, { msSinceBoot: a.msSinceBoot, epochS: a.epochS });
    }
  }
  const out = new Map<number, number>();
  for (const u of undated) {
    const anchor = u.bootCounter !== 0 ? byBoot.get(u.bootCounter) : undefined;
    if (anchor) {
      out.set(u.sequence, Math.round(anchor.epochS + (u.msSinceBoot - anchor.msSinceBoot) / 1000));
    }
  }
  return out;
}
