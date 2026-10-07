import { Chr } from '@/protocol/uuids';

import { loggable } from '../transport';

jest.mock('react-native-ble-manager', () => ({ __esModule: true, default: {}, BleState: {} }));

const bytes = Uint8Array.of(1, 2, 3);

test('key material never reaches a trace', () => {
  expect(loggable('read', Chr.enrolKey, bytes)).toBe('<redacted 3 bytes>');
  expect(loggable('write', Chr.enrolKey, bytes)).toBe('<redacted 3 bytes>');
  expect(loggable('write', Chr.auth, bytes)).toBe('<redacted 3 bytes>');
});

test('other values are traced as they are', () => {
  expect(loggable('read', Chr.auth, bytes)).toBe(bytes);
  expect(loggable('write', Chr.epoch, bytes)).toBe(bytes);
});
