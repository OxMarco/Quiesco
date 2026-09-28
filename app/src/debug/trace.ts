// Tagged console output for diagnosing the BLE link on a real phone. Lines
// start with [Quiesco] so scripts/device-debug.sh can pick them out of the
// device console; in development they also appear in the Metro terminal.

export function trace(tag: string, ...details: unknown[]) {
  const parts = details.map((d) => (typeof d === 'string' ? d : safeJson(d)));
  console.log(`[Quiesco] ${tag}`, ...parts);
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
