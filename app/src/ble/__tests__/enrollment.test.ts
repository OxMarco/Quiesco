import { setupKey } from '@/protocol/codec';

import { cancelEnrollment, enrollment, requestSetupKey, submitSetupKey } from '../enrollment';

afterEach(() => { cancelEnrollment(); jest.useRealTimers(); });

test('six digits resolve to the derived setup key', async () => {
  const pending = requestSetupKey(0x1234);
  expect(enrollment.get()).toEqual({ keyId: 0x1234 });
  expect(() => submitSetupKey('12345')).toThrow('six digits');
  submitSetupKey('123 456');
  await expect(pending).resolves.toEqual(setupKey('123456', 0x1234));
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
