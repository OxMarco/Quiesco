// The Android home-screen widget, drawn by react-native-android-widget from
// the same snapshot the iOS widget reads. Narrow: headline and CO₂; wide: the
// four values too. A tap opens the app.

'use no memo';

import { FlexWidget, TextWidget } from 'react-native-android-widget';

import { Palettes } from '@/theme';

import type { WidgetSnapshot, WidgetTone } from './snapshot';

type Palette = (typeof Palettes)['day'];

const NUMBER_FONT = 'Fredoka_600SemiBold';
const BODY_FONT = 'NotoSans_500Medium';

function toneColor(p: Palette, tone: WidgetTone): `#${string}` {
  const hex = tone === 'bad' ? p.bad : tone === 'warn' ? p.warn : tone === 'ok' ? p.primary : p.textMuted;
  return hex as `#${string}`;
}

const hex = (c: string) => c as `#${string}`;

function ago(ms: number, now: number): string {
  if (!ms) return '';
  const min = Math.round((now - ms) / 60_000);
  if (min < 1) return 'just now';
  if (min < 60) return `${min} min ago`;
  const h = Math.round(min / 60);
  return h < 24 ? `${h} h ago` : `${Math.round(h / 24)} d ago`;
}

function Value({ p, label, value }: { p: Palette; label: string; value: string }) {
  return (
    <FlexWidget style={{ flexDirection: 'column', flex: 1 }}>
      <TextWidget text={value} style={{ fontFamily: NUMBER_FONT, fontSize: 18, color: hex(p.text) }} />
      <TextWidget text={label} style={{ fontFamily: BODY_FONT, fontSize: 11, color: hex(p.textMuted) }} />
    </FlexWidget>
  );
}

function Room({ s, p, wide, now }: { s: WidgetSnapshot; p: Palette; wide: boolean; now: number }) {
  return (
    <FlexWidget
      clickAction="OPEN_APP"
      accessibilityLabel={`${s.unit}: ${s.headline}. ${s.summary}`}
      style={{
        height: 'match_parent',
        width: 'match_parent',
        flexDirection: 'column',
        justifyContent: 'space-between',
        backgroundColor: hex(p.surface),
        borderRadius: 22,
        padding: 14,
      }}>
      <FlexWidget style={{ flexDirection: 'row', justifyContent: 'space-between', width: 'match_parent' }}>
        <TextWidget text={s.unit} maxLines={1} truncate="END" style={{ fontFamily: BODY_FONT, fontSize: 12, color: hex(p.textMuted) }} />
        <TextWidget text={ago(s.measuredAt, now)} style={{ fontFamily: BODY_FONT, fontSize: 12, color: hex(p.textMuted) }} />
      </FlexWidget>
      <FlexWidget style={{ flexDirection: 'column' }}>
        <TextWidget
          text={s.headline}
          maxLines={2}
          truncate="END"
          style={{ fontFamily: BODY_FONT, fontSize: 15, fontWeight: '600', color: toneColor(p, s.tone) }}
        />
        {s.measuredAt ? (
          <FlexWidget style={{ flexDirection: 'row', alignItems: 'flex-end' }}>
            <TextWidget text={s.co2} style={{ fontFamily: NUMBER_FONT, fontSize: wide ? 30 : 34, color: hex(p.text) }} />
            <TextWidget text=" ppm" style={{ fontFamily: BODY_FONT, fontSize: 13, color: hex(p.textMuted), marginBottom: 6 }} />
          </FlexWidget>
        ) : null}
      </FlexWidget>
      {wide && s.measuredAt ? (
        <FlexWidget style={{ flexDirection: 'row', width: 'match_parent' }}>
          <Value p={p} label="Temperature" value={s.temperature} />
          <Value p={p} label="Humidity" value={s.humidity} />
          <Value p={p} label="Noise" value={s.noise} />
        </FlexWidget>
      ) : null}
    </FlexWidget>
  );
}

/** Light and dark, so the widget follows the launcher's theme. */
export function roomWidget(s: WidgetSnapshot, widthDp: number) {
  const now = Date.now();
  const wide = widthDp >= 250;
  return {
    light: <Room s={s} p={Palettes.day} wide={wide} now={now} />,
    dark: <Room s={s} p={Palettes.night} wide={wide} now={now} />,
  };
}
