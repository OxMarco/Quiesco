import { setupKey } from '@/protocol/codec';

import { Store } from './store';

/** v6 units show a six-digit code; v5 units showed the 32-digit key itself. */
export type SetupFormat = 'code' | 'key';

export const enrollment = new Store<{ keyId: number | null; format: SetupFormat }>({ keyId: null, format: 'code' });
let pending: {
  keyId: number;
  format: SetupFormat;
  resolve: (key: Uint8Array) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
} | null = null;

export function parseSetupCode(text: string, keyId: number): Uint8Array {
  const code = text.replace(/\s/g, '');
  if (!/^\d{6}$/.test(code)) throw new Error('Enter the six digits from the unit’s screen.');
  return setupKey(code, keyId);
}

export function parseSetupKey(text: string): Uint8Array {
  const hex = text.replace(/[\s-]/g, '');
  if (!/^[0-9a-fA-F]{32}$/.test(hex)) throw new Error('Enter all 32 digits from the unit’s screen. Use only 0–9 and A–F.');
  return Uint8Array.from(hex.match(/../g)!, (byte) => parseInt(byte, 16));
}

export function cancelEnrollment(message = 'Setup cancelled. Reconnect to try again.') {
  const current = pending;
  pending = null;
  enrollment.set({ keyId: null });
  if (current) { clearTimeout(current.timer); current.reject(new Error(message)); }
}

/** Resolves with the key to prove: derived from the code (v6) or typed in full (v5). */
export function requestSetupKey(keyId: number, format: SetupFormat = 'code'): Promise<Uint8Array> {
  cancelEnrollment();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => cancelEnrollment('Setup expired. Reconnect to get a new code.'), 150_000);
    pending = { keyId, format, resolve, reject, timer };
    enrollment.set({ keyId, format });
  });
}

export function submitSetupKey(text: string) {
  const current = pending;
  if (!current) throw new Error('Setup expired. Reconnect to try again.');
  const key = current.format === 'code' ? parseSetupCode(text, current.keyId) : parseSetupKey(text);
  pending = null;
  clearTimeout(current.timer);
  enrollment.set({ keyId: null });
  current.resolve(key);
}
