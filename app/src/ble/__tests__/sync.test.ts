// runSync and downloadBatch (link.ts) against a simulated unit that speaks
// PROTOCOL.md §7 byte for byte: START/ABORT on 0009, DATA/FRAGMENT/END on
// 000A, status on 0004. Each test checks what must hold in every scenario:
// nothing downloaded twice, the cursor right, the progress count never past
// its estimate, a lost packet costing one retry.

import { Capability, DeviceInfo } from '@/protocol/codec';
import { crc8, LOG_HEADER_BYTES, WIRE_RECORD_BYTES } from '@/protocol/log';
import { Chr } from '@/protocol/uuids';

import { link, subscribeLive, syncLog, SyncProgress } from '../link';

// ------------------------------------------------------------- the unit

interface UnitOptions {
  /** Sequences on flash, ascending; gaps model a wrapped ring or lost records. */
  log: number[];
  /**
   * Firmware before cd6b8b8: status says newest 0 until a measurement mounts
   * the log, and every START rides a measurement that appends one record.
   */
  oldFirmware?: boolean;
  /** Indices (counted over the whole test) of DATA/FRAGMENT packets lost in transit. */
  drop?: number[];
  /** Stop sending, without END, after this many packets of the first session. */
  stallAfter?: number;
}

const u16 = (b: Uint8Array, at: number, v: number) => new DataView(b.buffer).setUint16(at, v, true);
const u32 = (b: Uint8Array, at: number, v: number) => new DataView(b.buffer).setUint32(at, v, true);

function wireRecord(sequence: number): Uint8Array {
  const r = new Uint8Array(WIRE_RECORD_BYTES);
  u32(r, 0, sequence);
  u32(r, 48, 1); // boot counter
  return r;
}

function packet(type: 1 | 2 | 3, counter: number, sequence: number, count: number, fragment: number, remaining: number, body = new Uint8Array(0)) {
  const p = new Uint8Array(LOG_HEADER_BYTES + body.byteLength);
  p[0] = type;
  p[1] = counter & 0xff;
  u32(p, 2, sequence);
  p[6] = count;
  p[7] = fragment;
  u16(p, 8, Math.min(remaining, 0xffff));
  p.set(body, LOG_HEADER_BYTES);
  p[11] = crc8(p);
  return p;
}

class FakeUnit {
  log: number[];
  mounted: boolean;
  starts = 0;
  recordsSent = 0;
  private packetIndex = 0;
  private stream: { cancelled: boolean } | null = null;
  private subs = new Map<string, (data: Uint8Array) => void>();

  constructor(private o: UnitOptions) {
    this.log = [...o.log];
    this.mounted = !o.oldFirmware;
  }

  newest() {
    return this.log.length ? this.log[this.log.length - 1] : 0;
  }

  status(): Uint8Array {
    const s = new Uint8Array(20);
    u32(s, 12, this.mounted ? this.newest() : 0);
    s[16] = this.stream ? 2 : 0;
    return s;
  }

  async read(chr: string) {
    if (chr === Chr.status) return this.status();
    throw new Error(`unexpected read ${chr}`);
  }

  async write(chr: string, data: Uint8Array) {
    if (chr !== Chr.logSyncControl) throw new Error(`unexpected write ${chr}`);
    if (data[0] === 2) {
      if (this.stream) this.stream.cancelled = true;
      this.stream = null;
      return;
    }
    if (this.stream) return; // §7.1: a START during a download is ignored
    const view = new DataView(data.buffer, data.byteOffset);
    void this.run(view.getUint32(2, true), view.getUint16(6, true), view.getUint16(8, true));
  }

  async subscribe(chr: string, cb: (data: Uint8Array) => void) {
    this.subs.set(chr, cb);
    return () => this.subs.delete(chr);
  }

  private notify(chr: string, data: Uint8Array) {
    this.subs.get(chr)?.(data);
  }

  private async run(first: number, max: number, attPayload: number) {
    this.starts++;
    if (this.o.oldFirmware) {
      // The measurement the download rides: mounts the log, appends a record.
      this.mounted = true;
      this.log.push(this.newest() + 1);
    }
    const stream = { cancelled: false };
    this.stream = stream;
    this.notify(Chr.status, this.status());
    const payload = Math.min(180, Math.max(20, attPayload));
    const perPacket = Math.min(3, Math.floor((payload - LOG_HEADER_BYTES) / WIRE_RECORD_BYTES));
    const chunk = payload - LOG_HEADER_BYTES;
    const pending = this.log.filter((s) => s >= first).slice(0, max || undefined);
    const stallAt = this.starts === 1 ? this.o.stallAfter : undefined;
    let counter = 0;
    let lastSent = first > 0 ? first - 1 : 0;

    const send = async (p: Uint8Array) => {
      await Promise.resolve();
      if (stream.cancelled) return false;
      if (stallAt !== undefined && counter >= stallAt) {
        // Gone quiet: the unit's 10 s stall ends the session without END.
        this.stream = null;
        this.notify(Chr.status, this.status());
        return false;
      }
      counter++;
      if (!this.o.drop?.includes(this.packetIndex++)) this.notify(Chr.logSyncData, p);
      return true;
    };

    for (let i = 0; i < pending.length; ) {
      const remaining = this.newest() - lastSent;
      if (perPacket === 0) {
        const wire = wireRecord(pending[i]);
        const fragments = Math.ceil(WIRE_RECORD_BYTES / chunk);
        for (let f = 0; f < fragments; f++) {
          const body = wire.slice(f * chunk, (f + 1) * chunk);
          if (!(await send(packet(3, counter, pending[i], 1, f, remaining, body)))) return;
        }
        this.recordsSent += 1;
        i += 1;
      } else {
        const batch = pending.slice(i, i + perPacket);
        const body = new Uint8Array(batch.length * WIRE_RECORD_BYTES);
        batch.forEach((s, k) => body.set(wireRecord(s), k * WIRE_RECORD_BYTES));
        if (!(await send(packet(1, counter, batch[0], batch.length, 0, remaining, body)))) return;
        this.recordsSent += batch.length;
        i += batch.length;
      }
      lastSent = pending[i - 1];
    }
    await Promise.resolve();
    if (stream.cancelled) return;
    this.notify(Chr.logSyncData, packet(2, counter, lastSent + 1, 0, 0, 0));
    this.stream = null;
    this.notify(Chr.status, this.status());
  }
}

// ------------------------------------------------------------- the phone

let mockUnit: FakeUnit;
const mockDb = { cursor: 0, saved: [] as number[] };

jest.mock('../transport', () => ({
  read: (_id: string, _service: string, chr: string) => mockUnit.read(chr),
  write: (_id: string, _service: string, chr: string, data: Uint8Array) => mockUnit.write(chr, data),
  subscribe: (_id: string, _service: string, chr: string, cb: (data: Uint8Array) => void) => mockUnit.subscribe(chr, cb),
}));
jest.mock('../keys', () => ({}));
jest.mock('@/data/db', () => ({
  getUnit: async (serial: string) => ({ serial, peripheralId: 'P', name: 'Bench', cursor: mockDb.cursor, firmware: null, lastSyncMs: null }),
  saveRecords: async (_serial: string, records: { sequence: number }[]) => {
    mockDb.saved.push(...records.map((r) => r.sequence));
  },
  setCursor: async (_serial: string, cursor: number) => {
    mockDb.cursor = cursor;
  },
}));

const info: DeviceInfo = {
  protocolVersion: 6,
  capabilities: Capability.logDownload,
  firmware: '1.0.0',
  debugBuild: false,
  pairingOpen: false,
  bonded: true,
  noBattery: false,
  scd41Serial: 0,
  bootCounter: 1,
};

const range = (from: number, to: number) => Array.from({ length: to - from + 1 }, (_, i) => from + i);

/** Connects the phone to a fresh unit and records every sync state it shows. */
async function setUp(options: UnitOptions, cursor: number) {
  mockUnit = new FakeUnit(options);
  mockDb.cursor = cursor;
  mockDb.saved = [];
  link.set({ phase: 'ready', peripheralId: 'P', serial: 'S', info, status: null, sync: null });
  await subscribeLive();
  const states: SyncProgress[] = [];
  const stop = link.subscribe(() => {
    const s = link.get().sync;
    if (s && s !== states[states.length - 1]) states.push(s);
  });
  return { unit: mockUnit, states, stop };
}

function expectHonestProgress(states: SyncProgress[]) {
  for (const s of states) {
    if (s.expected !== null && s.state !== 'failed') expect(s.received).toBeLessThanOrEqual(s.expected);
  }
}

function expectSavedOnce(expected: number[]) {
  expect(new Set(mockDb.saved).size).toBe(mockDb.saved.length); // nothing saved twice
  expect([...mockDb.saved].sort((a, b) => a - b)).toEqual(expected);
}

afterEach(() => {
  jest.useRealTimers();
});

// ------------------------------------------------------------- scenarios

test('first sync: the whole log in batches, once', async () => {
  const { unit, states, stop } = await setUp({ log: range(1, 2500) }, 0);
  await expect(syncLog()).resolves.toBe(2500);
  stop();
  expectSavedOnce(range(1, 2500));
  expect(mockDb.cursor).toBe(2501);
  expect(unit.starts).toBe(2); // 2000 + 500
  expect(link.get().sync?.state).toBe('done');
  expectHonestProgress(states);
});

test('after a reboot, old firmware: only the new records, not the whole log', async () => {
  // Status says newest 0 until the first measurement; the app used to take
  // that for a replaced log and start again from 0 ("3,500 of 1 readings").
  const { unit, states, stop } = await setUp({ log: range(1, 3500), oldFirmware: true }, 3451);
  await syncLog();
  stop();
  expectSavedOnce(range(3451, 3501)); // the 50 it lacked, and the measurement the START rode
  expect(mockDb.cursor).toBe(3502);
  expect(unit.recordsSent).toBe(51);
  expectHonestProgress(states);
});

test('after a reboot, current firmware: only the new records', async () => {
  const { unit, states, stop } = await setUp({ log: range(1, 3500) }, 3451);
  await syncLog();
  stop();
  expectSavedOnce(range(3451, 3500));
  expect(unit.starts).toBe(1);
  expect(states.find((s) => s.state === 'waiting')?.expected).toBe(50);
  expectHonestProgress(states);
});

test('already up to date: one empty session', async () => {
  const { unit, stop } = await setUp({ log: range(1, 100) }, 101);
  await expect(syncLog()).resolves.toBe(0);
  stop();
  expect(mockDb.saved).toEqual([]);
  expect(unit.starts).toBe(1);
  expect(mockDb.cursor).toBe(101);
});

test('a lost packet costs one retry from the first missing record', async () => {
  // Payload 20: seven fragments a record, so packet 700 is inside record 101.
  const { unit, states, stop } = await setUp({ log: range(1, 300), drop: [700] }, 0);
  await syncLog();
  stop();
  expectSavedOnce(range(1, 300));
  expect(unit.starts).toBe(2);
  expect(mockDb.cursor).toBe(301);
  expectHonestProgress(states);
});

test('several lost packets across batches', async () => {
  const { unit, stop } = await setUp({ log: range(1, 2300), drop: [10, 9000, 15000] }, 0);
  await syncLog();
  stop();
  expectSavedOnce(range(1, 2300));
  expect(unit.starts).toBeLessThanOrEqual(2 + 3);
});

test('a wrapped ring: what it still holds, from its oldest', async () => {
  const { states, stop } = await setUp({ log: range(1001, 1600) }, 0);
  await syncLog();
  stop();
  expectSavedOnce(range(1001, 1600));
  expect(mockDb.cursor).toBe(1601);
  expectHonestProgress(states);
});

test('a replaced log (new flash): the cursor starts again from 0', async () => {
  const { stop } = await setUp({ log: range(1, 100) }, 5000);
  await syncLog();
  stop();
  expectSavedOnce(range(1, 100));
  expect(mockDb.cursor).toBe(101);
});

test('a stream that stalls without END picks up where it stopped', async () => {
  jest.useFakeTimers({ doNotFake: ['nextTick', 'setImmediate', 'queueMicrotask'] });
  const { unit, stop } = await setUp({ log: range(1, 200), stallAfter: 350 }, 0);
  let settled = false;
  const done = syncLog().finally(() => {
    settled = true;
  });
  for (let i = 0; i < 60 && !settled; i++) await jest.advanceTimersByTimeAsync(1000);
  await done;
  stop();
  expectSavedOnce(range(1, 200));
  expect(unit.starts).toBe(2);
});

test('a second sync while one runs joins it', async () => {
  const { unit, stop } = await setUp({ log: range(1, 50) }, 0);
  const [a, b] = await Promise.all([syncLog(), syncLog()]);
  stop();
  expect(a).toBe(50);
  expect(b).toBe(50);
  expect(unit.starts).toBe(1);
});
