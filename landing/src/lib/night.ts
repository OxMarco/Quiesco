// A sample night for the Nights preview: CO₂ every 10 minutes from 22:00 to
// 07:00, door shut at 23:00, a window opened at 05:30. Generated, not measured.

export interface Point {
  h: number; // hours after 22:00
  v: number; // ppm
}

export function sampleNight(): Point[] {
  const pts: Point[] = [];
  for (let i = 0; i <= 54; i++) {
    const h = i / 6;
    let v = h < 1 ? 560 + 30 * h : 590 + 780 * (1 - Math.exp(-(h - 1) / 3.2));
    if (h > 7.5) v -= (h - 7.5) * 260;
    v += Math.sin(i * 1.7) * 12 + Math.cos(i * 0.6) * 8;
    pts.push({ h, v: Math.round(v) });
  }
  return pts;
}

export function clock(h: number): string {
  const m = (Math.round(h * 60) + 22 * 60) % 1440;
  return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
}

/** First crossing of `limit`, how long the night stayed over it, and the peak. */
export function summarize(pts: Point[], limit: number) {
  const over = pts.filter((p) => p.v > limit);
  const first = over[0];
  const last = over[over.length - 1];
  const mins = Math.round((last.h - first.h) * 60) + 10;
  const peak = pts.reduce((a, b) => (b.v > a.v ? b : a));
  return {
    from: clock(first.h),
    duration: `${Math.floor(mins / 60)} h ${String(mins % 60).padStart(2, '0')} min`,
    peak,
  };
}
