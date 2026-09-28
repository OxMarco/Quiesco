// Scanning as external state: screens start and stop it, and read results
// through useStore, so no component sets state from an effect.

import { Store } from './store';
import * as ble from './transport';

interface ScanState {
  scanning: boolean;
  found: ble.Advert[];
  /** null until permissions have been asked. */
  permitted: boolean | null;
}

export const scanner = new Store<ScanState>({ scanning: false, found: [], permitted: null });

let stop: (() => void) | null = null;
let timer: ReturnType<typeof setTimeout> | null = null;
const SECONDS = 15;

export async function startScan() {
  stopScan();
  const permitted = await ble.requestPermissions();
  scanner.set({ permitted, found: [], scanning: permitted });
  if (!permitted) return;
  stop = await ble.scan((a) => {
    scanner.set((s) => ({
      found: s.found.some((x) => x.id === a.id) ? s.found : [...s.found, a].sort((x, y) => y.rssi - x.rssi),
    }));
  }, SECONDS);
  timer = setTimeout(() => scanner.set({ scanning: false }), SECONDS * 1000);
}

export function stopScan() {
  stop?.();
  stop = null;
  if (timer) clearTimeout(timer);
  timer = null;
  scanner.set({ scanning: false });
}
