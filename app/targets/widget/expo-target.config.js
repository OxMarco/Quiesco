// The iOS home- and lock-screen widget (WidgetKit), generated into ios/ at
// prebuild by @bacons/apple-targets. It reads the snapshot the app writes to
// the App Group (src/widgets/write.ios.ts). Colours are the app's palette
// (src/theme/index.ts), day and night.

/** @type {import('@bacons/apple-targets/app.plugin').ConfigFunction} */
module.exports = (config) => ({
  type: 'widget',
  name: 'QuiescoWidget',
  displayName: 'Quiesco',
  deploymentTarget: '17.0',
  entitlements: {
    'com.apple.security.application-groups': config.ios.entitlements['com.apple.security.application-groups'],
  },
  colors: {
    $widgetBackground: { light: '#F8F4EC', dark: '#182324' },
    $accent: { light: '#2F6468', dark: '#7DB3B3' },
    ink: { light: '#1E2B2D', dark: '#E8E4DA' },
    muted: { light: '#5E6B6B', dark: '#9AA5A4' },
    tide: { light: '#2F6468', dark: '#7DB3B3' },
    warn: { light: '#A87A00', dark: '#B5880C' },
    bad: { light: '#C83A62', dark: '#E35D82' },
  },
});
