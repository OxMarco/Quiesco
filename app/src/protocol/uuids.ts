// GATT identifiers (PROTOCOL.md §4).

const quiesco = (short: string) => `7A1E${short}-8E6F-4A7A-AE32-515549455343`;

export const QUIESCO_SERVICE = quiesco('0000');

export const Chr = {
  interval: quiesco('0001'),
  coreConfig: quiesco('0002'),
  reading: quiesco('0003'),
  status: quiesco('0004'),
  epoch: quiesco('0005'),
  displayScreen: quiesco('0006'),
  calibrationOffsets: quiesco('0007'),
  deviceName: quiesco('0008'),
  logSyncControl: quiesco('0009'),
  logSyncData: quiesco('000A'),
  calibrationControl: quiesco('000B'),
  deviceInfo: quiesco('000C'),
  deviceControl: quiesco('000D'),
  calibrationState: quiesco('000E'),
  diagnostics: quiesco('000F'),
  auth: quiesco('0010'),
  enrolKey: quiesco('0011'),
  sleepWindow: quiesco('0012'),
} as const;

export const DIS_SERVICE = '180A';

export const Dis = {
  manufacturer: '2A29',
  model: '2A24',
  serial: '2A25',
  firmware: '2A26',
  hardware: '2A27',
} as const;

/** The one protocol version this app speaks (§1); older units need new firmware. */
export const PROTOCOL_VERSION = 6;
