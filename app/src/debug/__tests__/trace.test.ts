import { redacted, trace, traceLines } from '../trace';

const g = globalThis as { __DEV__?: boolean };
const dev = g.__DEV__;
let log: jest.SpyInstance;

beforeEach(() => { log = jest.spyOn(console, 'log').mockImplementation(() => {}); });
afterEach(() => { log.mockRestore(); g.__DEV__ = dev; });

test('logs in development', () => {
  g.__DEV__ = true;
  trace('read', Uint8Array.of(1, 0xab));
  expect(log).toHaveBeenCalledWith('[Quiesco] read', '01 ab');
});

test('prints nothing in release builds, but keeps the line for a report', () => {
  g.__DEV__ = false;
  trace('release-read', Uint8Array.of(1, 2));
  expect(log).not.toHaveBeenCalled();
  expect(traceLines().at(-1)).toMatch(/^\d\d:\d\d:\d\d\.\d{3} release-read 01 02$/);
});

test('keeps only the newest lines', () => {
  g.__DEV__ = false;
  for (let i = 0; i < 450; i++) trace('line', String(i));
  const kept = traceLines();
  expect(kept).toHaveLength(400);
  expect(kept.at(-1)).toMatch(/line 449$/);
  expect(kept[0]).toMatch(/line 50$/);
});

test('redaction shows only the length', () => {
  expect(redacted(new Uint8Array(16))).toBe('<redacted 16 bytes>');
});
