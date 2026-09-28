// Golden vectors copied from firmware/src/protocol/PROTOCOL.md (asserted there
// by the firmware host tests), so app and firmware agree byte for byte.

import { fromHex, toHex } from '../bytes';
import {
  decodeCalibrationOffsets,
  decodeCalibrationState,
  decodeCoreConfig,
  decodeAuthState,
  decodeDeviceInfo,
  decodeEnrolKey,
  encodeEnrol,
  encodeProve,
  decodeDiagnostics,
  decodeInterval,
  decodeReading,
  decodeStatus,
  describeResetReason,
  deviceNameProblem,
  DisplayScreen,
  encodeCalibrationOffsets,
  encodeCoreConfig,
  encodeDeviceName,
  encodeEpoch,
  encodeFactoryReset,
  encodeFrc,
  encodeInterval,
  encodeLogSyncStart,
  FrcState,
  setupKey,
  TemperatureUnit,
  utf8Decode,
} from '../codec';
import {
  crc8,
  decodeRecord,
  inferEpochs,
  LogSession,
  logWasReplaced,
  parsePacketHeader,
  PacketType,
} from '../log';
import { hmacSha256, sha256 } from '../sha256';
import { batteryPercent, nudge, roundHalfAway, Severity, verdict } from '../comfort';

const G = {
  deviceInfo: '04 00 ff 0f 00 00 01 02 03 02 f6 e5 d4 c3 b2 a1 07 00 00 00',
  authState: '06 00 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 00 00',
  enrolKey: '34 12 00 00 a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af',
  prove: '02 00 34 12 f0 64 2a b3 02 06 0f 2f 61 13 6b 3f 1d ab 8a 1a',
  setupProve: '02 00 34 12 05 45 13 4e 24 62 bb 4d cb 21 37 60 44 ae ca 9e',
  interval: '2c 01 00 00',
  coreConfig: '05 00 00 00 2c 01 00 00 0a 00 00 00 01 02 00 00',
  reading: '66 08 ad 11 cd 8b 01 00 64 02 0c 07 00 00 82 01 48 0f 7f 00',
  status: '6f 00 fd 00 00 03 00 00 00 00 00 00 d2 04 00 00 01 03 e9 ff',
  calibrationState: '00 b9 55 69 a4 01 db ff 01 00 00 00',
  diagnostics: '02 00 00 00 10 0e 00 00 00 00 00 00',
  epoch: '00 a8 da 76 9b 01 00 00',
  offsets: '00 00 c0 bf 00 00 00 c0 00 00 40 40',
  syncStart: '01 00 b1 04 00 00 00 00 f4 00',
  frc: '01 00 a4 01',
  reset: '01 01 c7 fa',
  wireRecord:
    'b1 04 00 00 7f 00 01 00 10 c7 55 69 80 ee 36 00 00 00 00 00 00 00 ac 41 00 00 35 42 80 e6 c5 47 ' +
    '00 00 19 44 66 66 34 43 66 66 1a 42 35 5e 7a 40 07 00 00 00',
  dataHeader: '01 00 b1 04 00 00 01 00 01 00 00 06',
  fragment: '03 00 b1 04 00 00 01 00 01 00 00 35 b1 04 00 00 7f 00 01 00',
  end: '02 01 b2 04 00 00 00 00 00 00 00 ab',
};

describe('characteristic codecs', () => {
  test('device info', () => {
    expect(decodeDeviceInfo(fromHex(G.deviceInfo))).toEqual({
      protocolVersion: 4,
      capabilities: 0xfff,
      firmware: '1.2.3',
      debugBuild: false,
      pairingOpen: true,
      bonded: false,
      noBattery: false,
      scd41Serial: 0xa1b2c3d4e5f6,
      bootCounter: 7,
    });
  });

  test('charging and no-battery flags', () => {
    const status = fromHex(G.status);
    status[16] |= 8;
    expect(decodeStatus(status).charging).toBe(true);
    const info = fromHex(G.deviceInfo);
    info[9] |= 8;
    expect(decodeDeviceInfo(info).noBattery).toBe(true);
  });

  test('interval', () => {
    expect(toHex(encodeInterval(300))).toBe(G.interval);
    expect(decodeInterval(fromHex(G.interval))).toBe(300);
  });

  test('core config round-trips', () => {
    const config = decodeCoreConfig(fromHex(G.coreConfig));
    expect(config).toEqual({
      version: 5,
      intervalSeconds: 300,
      fullRefreshEvery: 10,
      bleAlwaysAvailable: true,
      displayScreen: DisplayScreen.Bento,
      temperatureUnit: TemperatureUnit.Celsius,
    });
    expect(toHex(encodeCoreConfig(config))).toBe(G.coreConfig);
    expect(toHex(encodeCoreConfig({ ...config, temperatureUnit: TemperatureUnit.Fahrenheit }))).toBe(
      '05 00 00 00 2c 01 00 00 0a 00 00 00 01 02 01 00',
    );
    expect(() => encodeCoreConfig({ ...config, intervalSeconds: 10 })).toThrow();
  });

  test('latest reading', () => {
    const r = decodeReading(fromHex(G.reading));
    expect(r.temperatureC).toBeCloseTo(21.5);
    expect(r.humidityPct).toBeCloseTo(45.25);
    expect(r.pressurePa).toBe(101325);
    expect(r.co2Ppm).toBe(612);
    expect(r.lux).toBeCloseTo(180.4);
    expect(r.noiseDb).toBeCloseTo(38.6);
    expect(r.batteryV).toBeCloseTo(3.912);
    expect(r.validMask).toBe(0x7f);
  });

  test('invalid fields read as null, never zero', () => {
    const bytes = fromHex(G.reading);
    bytes[18] = 0x7f & ~0x08; // CO2 bit clear
    const r = decodeReading(bytes);
    expect(r.co2Ppm).toBeNull();
    expect(r.temperatureC).not.toBeNull();
  });

  test('status', () => {
    const s = decodeStatus(fromHex(G.status));
    expect(s.presentMask).toBe(0xfd);
    expect(s.timedOutMask).toBe(0);
    expect(s.failures[1]).toBe(3);
    expect(s.newestSequence).toBe(1234);
    expect(s.clockSynced).toBe(true);
    expect(s.logDownloadActive).toBe(false);
    expect(s.charging).toBe(false);
    expect(s.frcState).toBe(FrcState.Done);
    expect(s.frcCorrectionPpm).toBe(-23);
  });

  test('epoch', () => {
    expect(toHex(encodeEpoch(Date.UTC(2026, 0, 1)))).toBe(G.epoch);
    expect(() => encodeEpoch(0)).toThrow();
  });

  test('calibration offsets round-trip', () => {
    const o = decodeCalibrationOffsets(fromHex(G.offsets));
    expect(o).toEqual({ noiseDb: -1.5, temperatureC: -2, humidityPct: 3 });
    expect(toHex(encodeCalibrationOffsets(o))).toBe(G.offsets);
    expect(() => encodeCalibrationOffsets({ ...o, temperatureC: 9 })).toThrow();
    expect(() => encodeCalibrationOffsets({ ...o, noiseDb: NaN })).toThrow();
  });

  test('commands', () => {
    expect(toHex(encodeLogSyncStart(1201, 0, 244))).toBe(G.syncStart);
    expect(toHex(encodeFrc(420))).toBe(G.frc);
    expect(() => encodeFrc(399)).toThrow();
    expect(toHex(encodeFactoryReset(true))).toBe(G.reset);
  });

  test('calibration state and diagnostics', () => {
    expect(decodeCalibrationState(fromHex(G.calibrationState))).toEqual({
      lastFrcEpochS: 1767225600,
      lastFrcReferencePpm: 420,
      lastFrcCorrectionPpm: -37,
      ascOff: true,
    });
    const d = decodeDiagnostics(fromHex(G.diagnostics));
    expect(d).toEqual({ resetReason: 2, uptimeSeconds: 3600, i2cBusStuck: false });
    expect(describeResetReason(d.resetReason)).toBe('watchdog');
    expect(describeResetReason(0)).toBe('power-on or brownout');
  });

  test('device names follow the firmware rules', () => {
    expect(deviceNameProblem('Bedroom')).toBeNull();
    expect(deviceNameProblem('')).not.toBeNull();
    expect(deviceNameProblem('a'.repeat(17))).not.toBeNull();
    expect(deviceNameProblem('two\nlines')).not.toBeNull();
    expect(deviceNameProblem('Camera da letto')).toBeNull(); // 15 bytes
    expect(deviceNameProblem('Schlafzimmer 😴')).not.toBeNull(); // 17 bytes
    expect(utf8Decode(encodeDeviceName('Chambre é'))).toBe('Chambre é');
  });
});

describe('log download', () => {
  // Build a packet the way the firmware does, sealing the CRC last.
  function seal(header: number[], body: Uint8Array = new Uint8Array()): Uint8Array {
    const p = new Uint8Array(12 + body.byteLength);
    p.set(header.slice(0, 12));
    p.set(body, 12);
    p[11] = 0;
    p[11] = crc8(p);
    return p;
  }

  test('CRC-8/SMBUS check value', () => {
    expect(crc8(Uint8Array.from('123456789', (c) => c.charCodeAt(0)))).toBe(0xf4);
  });

  test('record decodes', () => {
    const rec = decodeRecord(fromHex(G.wireRecord));
    expect(rec.sequence).toBe(1201);
    expect(rec.epochS).toBe(Date.UTC(2026, 0, 1, 1) / 1000);
    expect(rec.msSinceBoot).toBe(3_600_000);
    expect(rec.temperatureC).toBeCloseTo(21.5);
    expect(rec.co2Ppm).toBe(612);
    expect(rec.bootCounter).toBe(7);
  });

  test('DATA packet from the golden header and record', () => {
    const packet = fromHex(G.dataHeader + ' ' + G.wireRecord);
    expect(parsePacketHeader(packet)).toMatchObject({ type: PacketType.Data, sequence: 1201, recordCount: 1 });
    const session = new LogSession(1201);
    session.push(packet);
    session.push(fromHex(G.end));
    expect(session.records.map((r) => r.sequence)).toEqual([1201]);
    expect(session.ended).toBe(true);
    expect(session.nextCursor()).toBe(1202);
  });

  test('a corrupted packet is discarded', () => {
    const packet = fromHex(G.dataHeader + ' ' + G.wireRecord);
    packet[30] ^= 0xff;
    const session = new LogSession(1201);
    session.push(packet);
    expect(session.records).toHaveLength(0);
    expect(session.discarded).toBe(1);
    expect(session.nextCursor()).toBe(1201);
  });

  test('fragments at the 20-byte minimum reassemble', () => {
    const wire = fromHex(G.wireRecord);
    const session = new LogSession(1201);
    for (let i = 0; i < 7; i++) {
      const chunk = wire.slice(i * 8, i * 8 + 8);
      const packet = seal([3, i, 0xb1, 0x04, 0, 0, 1, i, 1, 0, 0, 0], chunk);
      if (i === 0) expect(toHex(packet)).toBe(G.fragment);
      session.push(packet);
    }
    expect(session.records).toHaveLength(1);
    expect(session.records[0].co2Ppm).toBe(612);
  });

  test('a missing fragment drops that record only', () => {
    const wire = fromHex(G.wireRecord);
    const session = new LogSession(1201);
    for (let i = 0; i < 7; i++) {
      if (i === 3) continue;
      session.push(seal([3, i, 0xb1, 0x04, 0, 0, 1, i, 1, 0, 0, 0], wire.slice(i * 8, i * 8 + 8)));
    }
    expect(session.records).toHaveLength(0);
    expect(session.discarded).toBeGreaterThan(0);
  });

  test('without END the cursor is one past the highest intact record', () => {
    const session = new LogSession(1201);
    session.push(fromHex(G.dataHeader + ' ' + G.wireRecord));
    expect(session.ended).toBe(false);
    expect(session.nextCursor()).toBe(1202);
  });

  test('gaps and replaced logs', () => {
    const session = new LogSession(1000);
    session.push(fromHex(G.dataHeader + ' ' + G.wireRecord));
    expect(session.hasGap).toBe(true);
    expect(logWasReplaced(1202, 1201)).toBe(false);
    expect(logWasReplaced(1202, 40)).toBe(true);
    expect(logWasReplaced(0, 0)).toBe(false);
  });

  test('undated records are dated from the same boot only', () => {
    const anchors = [{ bootCounter: 7, msSinceBoot: 3_600_000, epochS: 1_767_229_200 }];
    const undated = [
      { sequence: 1, bootCounter: 7, msSinceBoot: 600_000 },
      { sequence: 2, bootCounter: 6, msSinceBoot: 600_000 },
      { sequence: 3, bootCounter: 0, msSinceBoot: 600_000 },
    ];
    const dated = inferEpochs(undated, anchors);
    expect(dated.get(1)).toBe(1_767_229_200 - 3000);
    expect(dated.has(2)).toBe(false);
    expect(dated.has(3)).toBe(false);
  });
});

describe('comfort policy matches the panel', () => {
  const base = {
    temperatureC: 22,
    humidityPct: 45,
    pressurePa: null,
    co2Ppm: 600,
    lux: 3,
    noiseDb: 30,
    batteryV: 3.9,
    validMask: 0x7b,
  };

  test('rounding happens before judging', () => {
    expect(roundHalfAway(800.4)).toBe(800);
    expect(roundHalfAway(-0.5)).toBe(-1);
    expect(verdict({ ...base, co2Ppm: 800.4 }).severity).toBe(Severity.Ok);
    expect(verdict({ ...base, co2Ppm: 800.5 }).severity).toBe(Severity.Warn);
  });

  test('the worst metric wins; CO2 wins ties', () => {
    const v = verdict({ ...base, co2Ppm: 900, noiseDb: 60 });
    expect(v.metric).toBe('co2');
    expect(nudge(v)).toBe('Getting stuffy');
    const w = verdict({ ...base, co2Ppm: 900, temperatureC: 29 });
    expect(w.metric).toBe('temperature');
    expect(nudge(w)).toBe('Too hot');
  });

  test('light never judges; no valid metrics means unavailable', () => {
    expect(verdict({ ...base, lux: 50_000 }).severity).toBe(Severity.Ok);
    const none = verdict({ ...base, temperatureC: null, humidityPct: null, co2Ppm: null, noiseDb: null });
    expect(none.available).toBe(false);
  });

  test('battery curve', () => {
    expect(batteryPercent(3.3)).toBe(0);
    expect(batteryPercent(3.8)).toBe(50);
    expect(batteryPercent(3.875)).toBe(63);
    expect(batteryPercent(4.3)).toBe(100);
  });
});

describe('app-layer authentication (protocol v4)', () => {
  const challenge = Uint8Array.from({ length: 16 }, (_, i) => i);
  const key = Uint8Array.from({ length: 16 }, (_, i) => 0xa0 + i);
  const text = (t: string) => Uint8Array.from(t, (c) => c.charCodeAt(0));

  test('SHA-256 and HMAC match FIPS 180-4 and RFC 4231', () => {
    expect(toHex(sha256(text('abc'))).replace(/ /g, '')).toBe(
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    );
    expect(toHex(hmacSha256(new Uint8Array(20).fill(0x0b), text('Hi There'))).replace(/ /g, '')).toBe(
      'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7',
    );
    expect(toHex(hmacSha256(text('Jefe'), text('what do ya'), text(' want '), text('for nothing?'))).replace(/ /g, '')).toBe(
      '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843',
    );
    expect(
      toHex(hmacSha256(new Uint8Array(131).fill(0xaa), text('Test Using Larger Than Block-Size Key - Hash Key First'))).replace(/ /g, ''),
    ).toBe('60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54');
  });

  test('auth state', () => {
    const state = decodeAuthState(fromHex(G.authState));
    expect(state).toMatchObject({ authenticated: false, enrolmentOpen: true, enrolled: true });
    expect(toHex(state.challenge)).toBe(toHex(challenge));
  });

  test('enrolment key; zero key id means not ready', () => {
    const enrolled = decodeEnrolKey(fromHex(G.enrolKey));
    expect(enrolled?.keyId).toBe(0x1234);
    expect(toHex(enrolled!.key)).toBe(toHex(key));
    expect(decodeEnrolKey(new Uint8Array(20))).toBeNull();
    expect(toHex(encodeEnrol())).toBe('01 00');
  });

  test('prove write matches the firmware golden vector', () => {
    expect(toHex(encodeProve(0x1234, key, challenge))).toBe(G.prove);
  });

  test('v6 setup code 123456 proves as the firmware golden vector', () => {
    expect(toHex(encodeProve(0x1234, setupKey('123456', 0x1234), challenge))).toBe(G.setupProve);
    expect(() => setupKey('12345', 0x1234)).toThrow();
  });
});
