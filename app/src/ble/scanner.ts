// Scanning as external state: screens start and stop it, and read results
// through useStore, so no component sets state from an effect.

import { trace } from '@/debug/trace';

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
// Bumped by every start and stop, so a start that was stopped (or restarted)
// while awaiting permission or the radio drops its result.
let generation = 0;
const SECONDS = 15;

/** Never rejects: a failed scan just stops spinning. */
export async function startScan() {
  stopScan();
  const gen = ++generation;
  const permitted = await ble.requestPermissions().catch(() => false);
  if (gen !== generation) return;
  scanner.set({ permitted, found: [], scanning: permitted });
  if (!permitted) return;
  try {
    const stopNow = await ble.scan((a) => {
      scanner.set((s) => ({
        found: s.found.some((x) => x.id === a.id) ? s.found : [...s.found, a].sort((x, y) => y.rssi - x.rssi),
      }));
    }, SECONDS);
    if (gen !== generation) {
      stopNow();
      return;
    }
    stop = stopNow;
    timer = setTimeout(() => scanner.set({ scanning: false }), SECONDS * 1000);
  } catch (e) {
    trace('scan failed', e);
    if (gen === generation) scanner.set({ scanning: false });
  }
}

export function stopScan() {
  generation++;
  stop?.();
  stop = null;
  if (timer) clearTimeout(timer);
  timer = null;
  scanner.set({ scanning: false });
}
