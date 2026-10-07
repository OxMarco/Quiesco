import { setupKey } from '@/protocol/codec';

import { Store } from './store';

/** The unit on USB power shows a six-digit setup code (§3). */
export const enrollment = new Store<{ keyId: number | null }>({ keyId: null });
let pending: {
  keyId: number;
  resolve: (key: Uint8Array) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
} | null = null;

export function parseSetupCode(text: string, keyId: number): Uint8Array {
  const code = text.replace(/\s/g, '');
  if (!/^\d{6}$/.test(code)) throw new Error('Enter the six digits from the unit’s screen.');
  return setupKey(code, keyId);
}

export function cancelEnrollment(message = 'Setup cancelled. Reconnect to try again.') {
  const current = pending;
  pending = null;
  enrollment.set({ keyId: null });
  if (current) { clearTimeout(current.timer); current.reject(new Error(message)); }
}

/** Resolves with the key to prove, derived from the code the user types. */
export function requestSetupKey(keyId: number): Promise<Uint8Array> {
  cancelEnrollment();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => cancelEnrollment('Setup expired. Reconnect to get a new code.'), 150_000);
    pending = { keyId, resolve, reject, timer };
    enrollment.set({ keyId });
  });
}

export function submitSetupKey(text: string) {
  const current = pending;
  if (!current) throw new Error('Setup expired. Reconnect to try again.');
  const key = parseSetupCode(text, current.keyId);
  pending = null;
  clearTimeout(current.timer);
  enrollment.set({ keyId: null });
  current.resolve(key);
}
