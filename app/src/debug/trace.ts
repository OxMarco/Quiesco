// Tagged output for diagnosing the BLE link on a real phone. Every line goes
// to an in-memory ring the user can send to support (Settings → Send a
// diagnostic report); nothing is written to disk or sent on its own.
// Only development builds also print to the console, with lines starting
// [Quiesco] so scripts/device-debug.sh can pick them out: the device console
// is readable by other tools. Key material never reaches a trace (see
// transport.loggable).

const MAX_LINES = 400;
const lines: string[] = [];

export function trace(tag: string, ...details: unknown[]) {
  const parts = details.map((d) => (typeof d === 'string' ? d : safeJson(d)));
  lines.push(`${new Date().toISOString().slice(11, 23)} ${[tag, ...parts].join(' ')}`);
  if (lines.length > MAX_LINES) lines.splice(0, lines.length - MAX_LINES);
  if (__DEV__) console.log(`[Quiesco] ${tag}`, ...parts);
}

/** The newest lines, oldest first, each stamped with UTC time of day. */
export function traceLines(): readonly string[] {
  return lines.slice();
}

/** Stands in for key material in a trace, even in development. */
export function redacted(data: Uint8Array): string {
  return `<redacted ${data.byteLength} bytes>`;
}

function safeJson(value: unknown): string {
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  if (value instanceof Uint8Array) return Array.from(value, (b) => b.toString(16).padStart(2, '0')).join(' ');
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}
