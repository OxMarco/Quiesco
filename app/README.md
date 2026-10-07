# Quiesco app

The iOS and Android companion for the Quiesco sensor. It connects to a unit over
Bluetooth, shows the live reading, downloads the unit's log and charts each
night. Readings stay on the phone: there is no account and no server.

Built with Expo (SDK 57, Expo Router), NativeWind for styling,
`react-native-ble-manager` for Bluetooth and `expo-sqlite` for storage.

## Run it

Bluetooth needs native code, so the app runs as a development build, not in
Expo Go.

### Prerequisites

- Node.js 20.19+, 22.13+ or 24.3+ (what React Native 0.86 requires)
- **iOS:** macOS with Xcode and CocoaPods; a physical iPhone for Bluetooth
- **Android:** Android Studio with an SDK and a physical phone for Bluetooth

### Build and run

```sh
cd app
npm install
npx expo run:ios --device       # or: npx expo run:android --device
```

`expo run` generates the native `ios/` and `android/` projects on first use
(neither is checked in) and installs the development build on the phone.

### Make it your own

`app.json` holds identifiers tied to the original project. Change them before
building for your own phone:

| Key | Set it to |
|---|---|
| `expo.ios.bundleIdentifier` | a reverse-DNS ID you own, e.g. `com.yourname.quiesco` |
| `expo.ios.appleTeamId` | your Apple Developer team ID (Xcode → Settings → Accounts) |
| `expo.android.package` | the same reverse-DNS ID |

A free Apple ID is enough to run the app on your own iPhone; the build
expires after seven days and just needs reinstalling.

### Add a unit

1. Plug the unit into **USB power**: it only accepts new phones while powered.
2. In the app, tap **Add your Quiesco** and pick your unit.
3. Enter the **six-digit code** shown on the unit's e-ink panel.

The code never travels over Bluetooth. Once it is proved, the unit gives the
phone its own key, which the app keeps in secure storage; every later
connection proves it with a challenge-response. You can then unplug the unit. Authentication is not encryption: readings cross
the radio in the clear, but only an enrolled phone can read them or change
anything.

### Check your changes

```sh
npm test                          # protocol tests
npx tsc --noEmit && npx expo lint
```

### No hardware yet?

A simulator has no Bluetooth. The welcome screen offers **Explore with demo
data**, which adds a demo unit with three made-up nights so the screens can be
checked without hardware. It works in release builds too, and the demo can be
removed in Settings.

## Layout

| Path | Contents |
|---|---|
| `src/protocol/` | The BLE protocol in plain TypeScript: codecs, log packets and CRC, the comfort policy. Mirrors `firmware/src/protocol` and `firmware/src/ui/ComfortEvaluation`. |
| `src/protocol/__tests__/` | Golden vectors, checked against the examples in `firmware/src/protocol/PROTOCOL.md` |
| `src/ble/` | `transport.ts` wraps the Bluetooth library; `link.ts` runs the connect sequence, enrolment and authentication, confirmed writes and log download; `session.ts` reconnects and syncs |
| `src/data/` | SQLite store keyed by the unit's serial; the dev-only sample data |
| `src/app/` | Screens (Expo Router): Room, Nights, Unit tabs; Connect and Calibrate modals |
| `src/components/`, `src/ui/` | Shared components, metric formatting, night summaries |
| `src/theme/` | The palette, the single source for NativeWind variables and SVG colours |
| `brand/` | Logo sources and the identity explorations |

## How it follows the protocol

`firmware/src/protocol/PROTOCOL.md` is the contract. In short:

- It scans for the Quiesco service UUID, never for a name, and keys everything
  by the DIS serial number.
- It accepts protocol version 6 only, and refuses to talk to any other unit.
- It authenticates on every connection with the key it was given at
  enrolment. With no key, or a key the unit rejects, it asks the user to plug
  the unit into USB and enter the code on the panel again.
- It writes the time on every connection.
- It never splits a write, and it reads every config write back to confirm it.
- The log download keeps the cursor on the phone, reassembles fragments,
  drops packets that fail CRC-8, resets the cursor when the log was replaced,
  and dates pre-sync records from a synced record of the same boot.
- Verdicts use the firmware's comfort bands and rounding, so the phone and the
  e-ink panel always agree.

## Brand

Logo: the unit's e-ink face, asleep. Palette "Tide & Sand". Kiwi Maru for the
name, Fredoka for readings (the face the panel uses) and Noto Sans for text.
Comfortable readings use the tide colour, and only warn (amber) and bad (rose)
get their own colours, which always come with a word.

---

<sub>← Back to the [Quiesco overview](../README.md)</sub>
