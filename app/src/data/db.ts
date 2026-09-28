// Local store. Readings never leave the phone: there is no server, and the
// app keeps everything keyed by the unit's permanent serial (PROTOCOL.md §2).

import * as SQLite from 'expo-sqlite';

import type { LogRecord } from '@/protocol/log';
import { inferEpochs } from '@/protocol/log';

let dbPromise: Promise<SQLite.SQLiteDatabase> | null = null;

const SCHEMA_VERSION = 2;

async function migrate(db: SQLite.SQLiteDatabase) {
  await db.execAsync('PRAGMA journal_mode = WAL;');
  const row = await db.getFirstAsync<{ user_version: number }>('PRAGMA user_version');
  const version = row?.user_version ?? 0;
  if (version >= SCHEMA_VERSION) return;
  if (version < 1) {
    await db.execAsync(`
      CREATE TABLE IF NOT EXISTS units (
        serial TEXT PRIMARY KEY NOT NULL,
        peripheral_id TEXT NOT NULL,
        name TEXT NOT NULL,
        cursor INTEGER NOT NULL DEFAULT 0,
        firmware TEXT,
        added_ms INTEGER NOT NULL,
        last_sync_ms INTEGER
      );
      -- time_source: 1 = the device's clock, 2 = inferred from the same boot, NULL = cannot be dated
      CREATE TABLE IF NOT EXISTS records (
        serial TEXT NOT NULL,
        seq INTEGER NOT NULL,
        epoch_s INTEGER,
        time_source INTEGER,
        ms_since_boot INTEGER NOT NULL,
        boot INTEGER NOT NULL,
        valid INTEGER NOT NULL,
        temperature REAL, humidity REAL, pressure REAL, co2 REAL, lux REAL, noise REAL, battery REAL,
        PRIMARY KEY (serial, seq)
      ) WITHOUT ROWID;
      CREATE INDEX IF NOT EXISTS records_time ON records (serial, epoch_s);
    `);
  }
  if (version < 2) {
    // App preferences (the sleep window), stored as JSON by key.
    await db.execAsync(`
      CREATE TABLE IF NOT EXISTS settings (
        key TEXT PRIMARY KEY NOT NULL,
        value TEXT NOT NULL
      ) WITHOUT ROWID;
    `);
  }
  await db.execAsync(`PRAGMA user_version = ${SCHEMA_VERSION};`);
}

export function getDb(): Promise<SQLite.SQLiteDatabase> {
  dbPromise ??= SQLite.openDatabaseAsync('quiesco.db').then(async (db) => {
    await migrate(db);
    return db;
  });
  return dbPromise;
}

// ---------------------------------------------------------------- settings

export async function getSetting<T>(key: string): Promise<T | null> {
  const db = await getDb();
  const row = await db.getFirstAsync<{ value: string }>('SELECT value FROM settings WHERE key = ?', key);
  if (!row) return null;
  try {
    return JSON.parse(row.value) as T;
  } catch {
    return null;
  }
}

export async function setSetting(key: string, value: unknown) {
  const db = await getDb();
  await db.runAsync(
    'INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value',
    key,
    JSON.stringify(value),
  );
}

// ------------------------------------------------------------------- units

export interface Unit {
  serial: string;
  peripheralId: string;
  name: string;
  cursor: number;
  firmware: string | null;
  lastSyncMs: number | null;
}

interface UnitRow {
  serial: string;
  peripheral_id: string;
  name: string;
  cursor: number;
  firmware: string | null;
  last_sync_ms: number | null;
}

const toUnit = (r: UnitRow): Unit => ({
  serial: r.serial,
  peripheralId: r.peripheral_id,
  name: r.name,
  cursor: r.cursor,
  firmware: r.firmware,
  lastSyncMs: r.last_sync_ms,
});

export async function listUnits(): Promise<Unit[]> {
  const db = await getDb();
  const rows = await db.getAllAsync<UnitRow>('SELECT * FROM units ORDER BY added_ms');
  return rows.map(toUnit);
}

export async function getUnit(serial: string): Promise<Unit | null> {
  const db = await getDb();
  const row = await db.getFirstAsync<UnitRow>('SELECT * FROM units WHERE serial = ?', serial);
  return row ? toUnit(row) : null;
}

export async function upsertUnit(u: { serial: string; peripheralId: string; name: string; firmware: string }) {
  const db = await getDb();
  await db.runAsync(
    `INSERT INTO units (serial, peripheral_id, name, firmware, added_ms) VALUES (?, ?, ?, ?, ?)
     ON CONFLICT(serial) DO UPDATE SET peripheral_id = excluded.peripheral_id, name = excluded.name,
       firmware = excluded.firmware`,
    u.serial,
    u.peripheralId,
    u.name,
    u.firmware,
    Date.now(),
  );
}

export async function setCursor(serial: string, cursor: number, syncedAtMs: number) {
  const db = await getDb();
  await db.runAsync('UPDATE units SET cursor = ?, last_sync_ms = ? WHERE serial = ?', cursor, syncedAtMs, serial);
}

export async function renameUnit(serial: string, name: string) {
  const db = await getDb();
  await db.runAsync('UPDATE units SET name = ? WHERE serial = ?', name, serial);
}

/** Forget a unit and every reading taken from it. */
export async function forgetUnit(serial: string) {
  const db = await getDb();
  await db.withExclusiveTransactionAsync(async (tx) => {
    await tx.runAsync('DELETE FROM records WHERE serial = ?', serial);
    await tx.runAsync('DELETE FROM units WHERE serial = ?', serial);
  });
}

// ----------------------------------------------------------------- records

export async function saveRecords(serial: string, records: LogRecord[]): Promise<void> {
  if (records.length === 0) return;
  const db = await getDb();
  await db.withExclusiveTransactionAsync(async (tx) => {
    const stmt = await tx.prepareAsync(
      `INSERT OR IGNORE INTO records (serial, seq, epoch_s, time_source, ms_since_boot, boot, valid,
         temperature, humidity, pressure, co2, lux, noise, battery)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    );
    try {
      for (const r of records) {
        await stmt.executeAsync(
          serial,
          r.sequence,
          r.epochS,
          r.epochS !== null ? 1 : null,
          r.msSinceBoot,
          r.bootCounter,
          r.validMask,
          r.temperatureC,
          r.humidityPct,
          r.pressurePa,
          r.co2Ppm,
          r.lux,
          r.noiseDb,
          r.batteryV,
        );
      }
    } finally {
      await stmt.finalizeAsync();
    }
  });
  await dateUndated(serial);
}

/** Give undated records a time from a device-dated record of the same boot (§7.4). */
async function dateUndated(serial: string) {
  const db = await getDb();
  const undated = await db.getAllAsync<{ seq: number; boot: number; ms: number }>(
    'SELECT seq, boot, ms_since_boot AS ms FROM records WHERE serial = ? AND epoch_s IS NULL AND boot != 0',
    serial,
  );
  if (undated.length === 0) return;
  const boots = [...new Set(undated.map((u) => u.boot))];
  const anchors = await db.getAllAsync<{ boot: number; ms: number; epoch: number }>(
    // SQLite takes bare columns from the MIN(seq) row: the boot's first dated record.
    `SELECT boot, ms_since_boot AS ms, epoch_s AS epoch, MIN(seq) FROM records
     WHERE serial = ? AND time_source = 1 AND boot IN (${boots.map(() => '?').join(',')})
     GROUP BY boot`,
    serial,
    ...boots,
  );
  const dated = inferEpochs(
    undated.map((u) => ({ sequence: u.seq, bootCounter: u.boot, msSinceBoot: u.ms })),
    anchors.map((a) => ({ bootCounter: a.boot, msSinceBoot: a.ms, epochS: a.epoch })),
  );
  if (dated.size === 0) return;
  await db.withExclusiveTransactionAsync(async (tx) => {
    for (const [seq, epoch] of dated) {
      await tx.runAsync('UPDATE records SET epoch_s = ?, time_source = 2 WHERE serial = ? AND seq = ?', epoch, serial, seq);
    }
  });
}

export interface StoredPoint {
  t: number; // Unix seconds
  temperature: number | null;
  humidity: number | null;
  co2: number | null;
  lux: number | null;
  noise: number | null;
}

export async function recordsBetween(serial: string, fromS: number, toS: number): Promise<StoredPoint[]> {
  const db = await getDb();
  return db.getAllAsync<StoredPoint>(
    `SELECT epoch_s AS t, temperature, humidity, co2, lux, noise FROM records
     WHERE serial = ? AND epoch_s BETWEEN ? AND ? ORDER BY epoch_s`,
    serial,
    fromS,
    toS,
  );
}

export async function timeSpan(serial: string): Promise<{ first: number; last: number; count: number } | null> {
  const db = await getDb();
  const row = await db.getFirstAsync<{ first: number | null; last: number | null; count: number }>(
    'SELECT MIN(epoch_s) AS first, MAX(epoch_s) AS last, COUNT(*) AS count FROM records WHERE serial = ? AND epoch_s IS NOT NULL',
    serial,
  );
  return row && row.first !== null && row.last !== null ? { first: row.first, last: row.last, count: row.count } : null;
}

/** The newest dated record, shown when the unit is out of range. */
export async function latestPoint(serial: string): Promise<StoredPoint | null> {
  const db = await getDb();
  return db.getFirstAsync<StoredPoint>(
    `SELECT epoch_s AS t, temperature, humidity, co2, lux, noise FROM records
     WHERE serial = ? AND epoch_s IS NOT NULL ORDER BY epoch_s DESC LIMIT 1`,
    serial,
  );
}

/**
 * Nights with data, newest first, as local dates (YYYY-MM-DD) of the evening
 * that starts them: a reading belongs to the night of (its time − 12 h).
 */
export async function nightsWithData(serial: string, limit = 60): Promise<string[]> {
  const db = await getDb();
  const rows = await db.getAllAsync<{ night: string }>(
    `SELECT DISTINCT date(epoch_s - 43200, 'unixepoch', 'localtime') AS night FROM records
     WHERE serial = ? AND epoch_s IS NOT NULL ORDER BY night DESC LIMIT ?`,
    serial,
    limit,
  );
  return rows.map((r) => r.night);
}
