// Colours are CSS variables set per scheme from src/theme/palette.ts (see
// ThemeRoot), so one class like `bg-surface` is Sand by day and Kelp by night.
const token = (name) => `rgb(var(--${name}) / <alpha-value>)`;

/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ['./src/**/*.{ts,tsx}'],
  presets: [require('nativewind/preset')],
  theme: {
    extend: {
      colors: {
        bg: token('bg'),
        surface: token('surface'),
        raised: token('raised'),
        line: token('line'),
        ink: token('text'),
        muted: token('text-muted'),
        primary: token('primary'),
        'on-primary': token('on-primary'),
        warn: token('warn'),
        bad: token('bad'),
        driftwood: token('driftwood'),
      },
      fontFamily: {
        wordmark: ['KiwiMaru_300Light'],
        title: ['KiwiMaru_400Regular'],
        number: ['Fredoka_600SemiBold'],
        'number-medium': ['Fredoka_500Medium'],
        body: ['NotoSans_400Regular'],
        'body-medium': ['NotoSans_500Medium'],
        'body-semi': ['NotoSans_600SemiBold'],
      },
      borderRadius: { card: '22px', tile: '16px' },
    },
  },
  plugins: [],
};
