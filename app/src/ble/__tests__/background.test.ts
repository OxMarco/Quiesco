// What session.ts does with the link once the app leaves the screen: drop it
// so other phones can reach the unit, and not take it straight back.

import { AppState } from 'react-native';

import { backgroundSync, boot } from '../session';

const mockState = { app: 'active', listeners: [] as ((s: string) => void)[], inRange: true, connects: 0, waits: 0, disconnects: 0 };

jest.mock('expo-application', () => ({ getInstallationTimeAsync: async () => null }));
jest.mock('expo-store-review', () => ({ isAvailableAsync: async () => false }));
jest.mock('@/health/sleep', () => ({}));
jest.mock('@/debug/trace', () => ({ trace: () => {} }));
jest.mock('@/ui/units', () => ({ localeTempUnit: () => 'C' }));
jest.mock('../keys', () => ({ loadKey: async () => 'key', deleteKey: async () => {} }));
jest.mock('@/data/db', () => {
  const unit = { serial: 'S1', peripheralId: 'P1', name: 'Bench', cursor: 0, firmware: null, lastSyncMs: null };
  return {
    __esModule: true,
    getSetting: async () => null,
    setSetting: async () => {},
    listUnits: async () => [unit],
    getUnit: async () => unit,
    nightsWithData: async () => [],
  };
});
jest.mock('../transport', () => ({
  __esModule: true,
  onRadioState: () => {},
  radioState: async () => 'on',
  requestPermissions: async () => true,
  hasPermissions: async () => true,
  // A unit in range advertises whenever it has no link, so the system connects at once.
  waitFor: () => {
    mockState.waits++;
    return mockState.inRange ? new Promise((r) => setTimeout(r, 5)) : new Promise(() => {});
  },
}));
jest.mock('../link', () => {
  const { Store } = jest.requireActual('../store');
  const initial = { phase: 'idle', peripheralId: null, serial: null, info: null, sync: null, config: null, status: null };
  const link = new Store({ ...initial });
  return {
    __esModule: true,
    link,
    connect: async (id: string) => {
      mockState.connects++;
      link.set({ ...initial, phase: 'connecting', peripheralId: id });
      await new Promise((r) => setTimeout(r, 5));
      if (mockState.inRange) link.set({ phase: 'ready', serial: 'S1' });
      else link.set({ phase: 'error' });
    },
    disconnect: async () => {
      mockState.disconnects++;
      await Promise.resolve();
      link.set({ ...initial });
    },
    syncLog: async () => {
      link.set({ sync: { state: 'done' } });
      return 0;
    },
    describeError: (e: unknown) => String(e),
  };
});

Object.defineProperty(AppState, 'currentState', { get: () => mockState.app, configurable: true });
jest.spyOn(AppState, 'addEventListener').mockImplementation(((_: string, cb: (s: string) => void) => {
  mockState.listeners.push(cb);
  return { remove() {} };
}) as never);

const settle = (ms = 200) => new Promise((r) => setTimeout(r, ms));

describe('in the background', () => {
  it('drops the link once and does not take it back', async () => {
    await boot();
    await settle();
    expect(mockState.connects).toBe(1);

    mockState.app = 'background';
    mockState.listeners.forEach((l) => l('background'));
    await settle();
    expect(mockState).toMatchObject({ connects: 1, disconnects: 1, waits: 0 });
  });

  it('syncs once per periodic run, then frees the unit again', async () => {
    await backgroundSync('periodic');
    await settle();
    expect(mockState).toMatchObject({ connects: 2, disconnects: 2, waits: 0 });
  });

  it('waits for a unit that is out of range', async () => {
    mockState.inRange = false;
    await backgroundSync('periodic');
    await settle();
    expect(mockState).toMatchObject({ connects: 3, disconnects: 2, waits: 1 });
  });
});
