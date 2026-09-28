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
