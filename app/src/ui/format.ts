/**
 * Sentences as one string. A lone sentence gets no full stop (UI copy style);
 * two or more are punctuated normally. Falsy parts are skipped.
 */
export function prose(...sentences: (string | null | false | undefined)[]): string {
  const parts = sentences.filter((s): s is string => !!s);
  return parts.length === 1 ? parts[0] : parts.map((s) => `${s}.`).join(' ');
}

export function ago(ms: number | null, now = Date.now()): string {
  if (ms === null) return 'never';
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 45) return 'just now';
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.round(m / 60);
  if (h < 24) return `${h} h ago`;
  return new Date(ms).toLocaleDateString([], { day: 'numeric', month: 'short' });
}

export function intervalLabel(seconds: number): string {
  return seconds < 3600 ? `${seconds / 60} min` : `${seconds / 3600} h`;
}
