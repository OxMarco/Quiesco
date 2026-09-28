import { setupKey } from '@/protocol/codec';

import { cancelEnrollment, enrollment, parseSetupKey, requestSetupKey, submitSetupKey } from '../enrollment';

afterEach(() => { cancelEnrollment(); jest.useRealTimers(); });

test('accepts all four displayed rows, preserving leading zeroes', () => {
  expect(Array.from(parseSetupKey('0001 0203\n0405 0607\n0809 0a0b\n0c0d 0e0f'))).toEqual(Array.from({ length: 16 }, (_, i) => i));
  expect(() => parseSetupKey('0011')).toThrow();
  expect(() => parseSetupKey('g'.repeat(32))).toThrow();
});

test('v6: six digits resolve to the derived setup key', async () => {
  const pending = requestSetupKey(0x1234);
  expect(enrollment.get()).toEqual({ keyId: 0x1234, format: 'code' });
  expect(() => submitSetupKey('12345')).toThrow('six digits');
  submitSetupKey('123 456');
  await expect(pending).resolves.toEqual(setupKey('123456', 0x1234));
});

test('v5: keeps the secret out of observable UI state and resolves on submission', async () => {
  const pending = requestSetupKey(0x1234, 'key');
  expect(enrollment.get()).toEqual({ keyId: 0x1234, format: 'key' });
  submitSetupKey('00'.repeat(16));
  await expect(pending).resolves.toEqual(new Uint8Array(16));
  expect(enrollment.get().keyId).toBeNull();
});

test('disconnect and timeout reject the pending request', async () => {
  jest.useFakeTimers();
  const disconnected = requestSetupKey(1);
  const rejected = expect(disconnected).rejects.toThrow('disconnected');
  cancelEnrollment('disconnected');
  await rejected;
  const expired = requestSetupKey(2);
  const timeout = expect(expired).rejects.toThrow('expired');
  jest.advanceTimersByTime(150_000);
  await timeout;
  expect(enrollment.get().keyId).toBeNull();
});
