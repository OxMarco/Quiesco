# Quiesco app

The iOS and Android companion for the Quiesco sensor. It connects to a unit over
Bluetooth, shows the live reading, downloads the unit's log and charts each
night. It holds any number of units, shows one at a time, and syncs the others
in range quietly when it opens. Readings stay on the phone: there is no account and no server.

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
npm test                          # protocol, sync and UI tests (jest)
npx tsc --noEmit && npx expo lint
```

### Before a release: licences

Settings → About → Open-source licences lists every app dependency and every
firmware component with its licence, from `src/data/licences.json`. Refresh it
whenever dependencies change, and before every release:

```sh
(cd ../firmware && make licences)  # the firmware's licence texts
npm run licences                   # writes src/data/licences.json
```

## Layout

| Path | Contents |
|---|---|
| `src/protocol/` | The BLE protocol in plain TypeScript: codecs, log packets and CRC, the comfort policy. Mirrors `firmware/src/protocol` and `firmware/src/ui/ComfortEvaluation`. |
| `src/protocol/__tests__/` | Golden vectors, checked against the examples in `firmware/src/protocol/PROTOCOL.md` |
| `src/ble/` | `transport.ts` wraps the Bluetooth library; `link.ts` runs the connect sequence, enrolment and authentication, confirmed writes and log download; `session.ts` reconnects and syncs |
| `src/data/` | SQLite store keyed by the unit's serial |
| `src/background/` | The periodic background sync task (expo-background-task) |
| `src/health/` | Sleep from Apple Health (`sleep.ios.ts`) and Health Connect (`sleep.android.ts`), read only |
| `src/widgets/` | The widget snapshot, its writers and the Android widget (`RoomWidget.tsx`) |
| `targets/widget/` | The iOS widget (SwiftUI, WidgetKit), generated into `ios/` by `@bacons/apple-targets` |
| `index.ts` | App entry: defines the background task and the Android widget handler before the router starts |
| `src/app/` | Screens (Expo Router): Room, Nights and Settings tabs; one page per unit (`units/[serial]`); the unit switcher, Connect, Name it, Calibrate, Sleep window, metric detail and 'why' screens |
| `src/components/`, `src/ui/` | Shared components, metric formatting, night summaries |
| `src/theme/` | The palette, the single source for NativeWind variables and SVG colours |
| `brand/` | Logo sources and the identity explorations |

## Background sync, sleep times and widgets

**Background sync.** The phone keeps each unit's clock set and its log synced
without the app being opened:

- A periodic task (expo-background-task) syncs the unit on screen. Android
  runs it with WorkManager, every 15 minutes at best; iOS with
  BGTaskScheduler, when the system chooses, often overnight on charge.
- When the app leaves the screen with the unit out of range, it asks the
  system to connect when the unit comes back: on iOS a pending connect,
  which wakes or relaunches the app (`bluetooth-central` and state
  restoration); on Android an autoconnect, while the app's process lives.
- After a background sync the link stays up only while the unit is on USB
  power, so live readings keep the widgets current; on battery it is
  dropped, because a held connection drains the unit.
- A background run never asks for anything: a unit without a key on this
  phone, or Bluetooth without permission, is skipped.

**Sleep times.** With Settings → Sleep times from Apple Health (Health
Connect on Android) on, each night is judged over the sleep the health store
recorded, where it has a record, and the sleep window offers the usual times
over the last three weeks. Read only; nothing is written or sent.

**Widgets.** One snapshot (the unit, its newest reading, the Room tab's
headline) drives both: the iOS widget reads it from the App Group
`group.rest.quiesco.app`, the Android widget is drawn from it in JS. The app
refreshes them on every new reading, at most once a minute.

### Android and iOS

| Feature | iOS | Android |
|---|---|---|
| Periodic background sync | BGTaskScheduler (system-chosen, often overnight) | WorkManager (≥ 15 min, later in Doze) |
| Reconnect when the unit comes back | Pending connect; relaunches the app if it was closed | Autoconnect while the process lives; else the periodic task |
| Link kept up in the background on USB power | Yes (`bluetooth-central`) | Yes, while the process lives |
| Sleep times | Apple Health (HealthKit sleep analysis) | Health Connect (sleep sessions) |
| Home-screen widget | Small and medium | Resizable, narrow and wide layouts |
| Lock-screen widget | Rectangular and inline | Not available on Android |
| Widget refresh | App reloads the timeline on new data; hourly entry | App redraws on new data; every 30 min by the system |

Testing: background tasks run only on a device on iOS (not the simulator);
on Android, background the app and force the job with
`adb shell cmd jobscheduler run -f rest.quiesco.app <job>`. Neither
simulator nor emulator has Bluetooth, so syncing needs a phone and a unit.

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
