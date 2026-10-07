// Enrolment keys (PROTOCOL.md §3), one per unit serial, in the iOS Keychain /
// Android Keystore via expo-secure-store. Never in SQLite with the readings.

import * as SecureStore from 'expo-secure-store';

import { trace } from '@/debug/trace';
import { fromHex, toHex } from '@/protocol/bytes';
import type { EnrolledKey } from '@/protocol/codec';

const storeKey = (serial: string) => `quiesco.unit.${serial}`;

export async function loadKey(serial: string): Promise<EnrolledKey | null> {
  let raw: string | null;
  try {
    raw = await SecureStore.getItemAsync(storeKey(serial));
  } catch (e) {
    // Undecryptable (restored backup, reset Keystore): as good as no key.
    trace('key unreadable', e);
    return null;
  }
  if (!raw) return null;
  try {
    const { keyId, key } = JSON.parse(raw) as { keyId: number; key: string };
    return { keyId, key: fromHex(key) };
  } catch {
    return null;
  }
}

export async function saveKey(serial: string, enrolled: EnrolledKey): Promise<void> {
  await SecureStore.setItemAsync(
    storeKey(serial),
    JSON.stringify({ keyId: enrolled.keyId, key: toHex(enrolled.key) }),
    { keychainAccessible: SecureStore.AFTER_FIRST_UNLOCK_THIS_DEVICE_ONLY },
  );
}

export async function deleteKey(serial: string): Promise<void> {
  await SecureStore.deleteItemAsync(storeKey(serial));
}
