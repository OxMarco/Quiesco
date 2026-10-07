# Quiesco v1 — Display UI

The e-ink panel has two jobs: show the **readings** for someone standing at the
device, and give a **verdict** that reads from across the room. Every metric is
measured and synced on every cycle regardless of the screen; the glass only
repeats what is useful.

Design reference: <https://claude.ai/code/artifact/9b974cad-2319-4fad-8843-c2d7321627a3>

---

## 1. Canvas

- **Panel:** GDEW0154T8D, 152 × 152 px, 1-bit black on white. All geometry
  below is in panel pixels, origin top-left.
- **Fonts:** Fredoka, converted to Adafruit GFX bitmaps — Regular 7/9 pt,
  SemiBold 7/9/12/24 pt. The fonts are ASCII-only, so `CO2` has no subscript
  and a degree sign is drawn as a two-ring circle (written `*` in layout
  strings). The 24 pt cut carries only digits, `-` and `.` (~1.2 KB).
- **Chassis inset:** the case window sits low over the panel and hides the top
  rows. `kTopInset` = **7 px** (1.27 mm at 0.182 mm/px) is a floor for layouts
  that would otherwise be clipped — ledger, bento, battery and unavailable.
  It is not a blanket shift: the face's topmost ink is already at y = 24, and
  ink that deliberately bleeds to the edge (an inverted bento hero) starts at
  y = 0 so a shifted case can never reveal a white sliver. Re-measure if the
  case changes.

---

## 2. Choosing the screen

The configured screen is `Config::displayScreen`, set over BLE (characteristic
`0006`) and persisted: **face**, **ledger** or **bento**. Bento is the factory
default. Two screens override the configured one:

| Screen | Shown when |
|---|---|
| **Battery** | the charger reports charging, or the battery reads below **3.5 V** |
| **Unavailable** | the face is configured but CO2, temperature, humidity and noise are all invalid |

Plugging or unplugging USB starts a measurement cycle immediately (after a 1 s
debounce), so the battery screen appears and disappears without waiting for
the next interval.

Temperatures are drawn in °C or °F per `Config::temperatureUnit` (core config
byte 14, which the app writes to match its own setting). Only the digits
change: the reading, the comfort bands and the ledger gauge stay in °C.

A device that already has a stored config keeps its screen through a firmware
update. Changing it means the BLE characteristic, an erased config partition,
or a `Config::kCurrentVersion` bump (which resets every setting).

---

## 3. Comfort bands

One stateless policy (`ui/ComfortEvaluation`) judges every metric for every
screen. A metric is **comfortable**, **warn** or **bad**. The table is the
**sleep** mode, which applies at every hour unless the panel follows the
sleep window (§3.1):

| Metric | Comfortable | Warn | Bad | Nudge (warn / bad) |
|---|---|---|---|---|
| CO2 | ≤ 800 ppm | 800–1200 ppm | > 1200 ppm | "getting stuffy" / "open a window" |
| Temperature | 20–26 °C | 18–20 or 26–28 °C | < 18 or > 28 °C | "a bit chilly" · "a bit warm" / "too cold" · "too hot" |
| Humidity | 30–60 % | 25–30 or 60–70 % | < 25 or > 70 % | "a bit dry" · "a bit damp" / "too dry" · "too humid" |
| Noise | ≤ 55 dB | 55–70 dB | > 70 dB | "a bit loud" / "too loud" |
| Light | informational only — never judged, never drives the face | | | |

Values are **rounded before judging**, so the band a reading falls in always
matches the number printed next to it. The **worst** valid metric sets the
verdict; on a tie the order is CO2, temperature, humidity, noise. Noise is
dB(A), an A-weighted Leq over ~7 s, the unit the 55/70 dB bands are meant
in.

Invalid metrics never drive a verdict or invert a tile. They render as `n/a`
(ledger, with no marker) or `--` (bento).

### 3.1 By time of day, like the app

When the app turns it on (sleep window `0012`, `PROTOCOL.md` §6.17) and the
clock has been synced since boot, the panel judges the room the way the app
does (`ui/SleepSchedule`, a port of the app's `judgeMode` tested for parity
against it): **sleep** mode from 60 minutes before bedtime until wake, **day**
mode otherwise. Without the window, or before the clock is synced, it is sleep
mode at every hour, as above.

| Metric | Day mode | Nudge by day (warn / bad) |
|---|---|---|
| CO2 | same band as at night | "getting stuffy" / "open a window" |
| Noise | hearing band: ≤ 70 comfortable, 70–85 warn, > 85 bad dB(A) | "loud" / "very loud" |
| Temperature, humidity | shown, **not judged**: never a frown, a nudge or an inverted tile, and no band on the ledger gauge | — |
| Light | never judged, as at night | — |

The day noise band is the app's (`DAY_NOISE`, from the EPA's 70 dB 24-hour
average and NIOSH's 85 dB(A) 8-hour limit), and the day nudges are its
"Loud" / "Very loud", lowercased like every panel nudge. Every nudge, day and
night, fits the panel at 9 pt Regular: the widest is "open a window" at
122 px; "very loud" is 75 px (measured from the font bitmaps against the
152 px panel).

The panel changes mode at its next draw after the boundary, so it can lag the
app by up to one measurement interval. A sleep-window write that changes the
mode at once redraws immediately.

---

## 4. Screens

### Face — "speak only when wrong"

Blank paper and a large smiley. A metric earns space on screen only by leaving
its comfort band.

- **Face:** circle r = 40 at (76, 64) with a 5 px stroke; eyes r = 4 at
  (62, 56) and (90, 56). Only the mouth changes — no brows, because a face that
  furrows at you nags:
  - everything comfortable → smile, and nothing else on screen;
  - worst is warn → flat mouth;
  - worst is bad → frown.
- **Callout** (warn or bad only), baseline y = 126: the worst metric's value in
  **12 pt SemiBold**, its name and unit in 9 pt beside it — the number is what
  you act on. CO2 is the only metric that keeps a name (`CO2 803 ppm`), because
  `ppm` does not identify it the way `°C` and `%` do. A line wider than 148 px
  (only a five-digit CO2 value from a broken sensor) drops to 9 pt instead of
  clipping.
- **Nudge**, baseline y = 145, 9 pt Regular — the phrase from §3.

### Ledger — readings with comfort bands

One row per metric, each with a label, a value and a gauge that carries the
judgement.

- Five rows at a 28 px pitch, first baseline y = 23 (16 + the inset), judged
  metrics first: CO2, temperature, humidity, noise, then light.
- Label left in **7 pt Regular**, value right-aligned in **7 pt SemiBold**.
  This is the only layout with a label and a value on one 132 px line; at 9 pt
  a five-digit CO2 or lux value ran into its label. 7 pt leaves 27 px clear at
  the worst case.
- **Gauge** at baseline + 9, x 10 → 142. The full scale is a **dotted** 1 px
  track — a 1-bit panel has no grey, so dotting is the only way it recedes
  behind the solid 3 px comfort band drawn over it. The marker is an r = 4
  black disc with an r = 2 white pupil: inside the band is fine, and its
  distance outside the band is the only fault signal the row needs.
- **Scales:** CO2 400–2000 ppm, temperature 10–35 °C, humidity 0–100 %,
  noise 30–100 dB, light log₁₀ over 1–10 000 lux.
- **Light has no band.** It is never judged, so a band would put the marker
  outside it in a bright room and read as a fault no other screen agrees with.
  For the same reason temperature and humidity lose their bands in day mode,
  and noise shows the day band (§3.1).

### Bento — biggest numerals (default)

Five tiles, readable from twice the distance of the ledger. No face.

- **CO2 hero**, full width, y 0–59: value in **24 pt** on baseline y = 41,
  label `CO2 PPM` on y = 57. The 24 pt cut has no letters, so this is the one
  tile whose unit lives in its label.
- **2 × 2 grid** below — dividers at y = 60, y = 106 and x = 76 — temperature,
  humidity, light and noise in 76 × 46 tiles. Value in **12 pt** on y + 22,
  label in 9 pt on y + 40. The dividers sit 4 px lower than the first draft so
  the hero keeps a 33 px numeral below the inset while the bottom row still
  ends on the panel.
- Tile values carry their unit (`22°C`, `44%`, `320 lx`, `41 dB`), so every
  label is a bare metric name that fits the 76 px cell. Light abbreviates
  above 999 (`12k lx`), because a sunlit reading is six digits wide.
- An **out-of-band tile inverts** to white on black, warn and bad alike — the
  bento's version of the ledger's gauge.

### Battery — charging or nearly empty

Not selectable; overrides the configured screen (§2).

- A battery outline 116 × 58 px with a 5 px stroke and a terminal nub, filled
  in proportion to charge. The percentage comes from a 1S LiPo resting-voltage
  curve (3.30 V = 0 % … 4.20 V = 100 %) and is rounded to **5 % steps** to limit
  refreshes.
- While charging, a lightning bolt is drawn in white with a black outline, so
  it stays visible across the fill edge.
- No number is shown, here or in the app: while charging, the voltage reads
  the charger's output and overstates the charge.
- A bench unit without a cell is built with `scripts/build.sh --no-battery`.
  It never shows this screen and reports no battery to the app.

### Phone setup — a phone is enrolling

Not selectable; overrides every other screen while a USB-powered enrollment
is pending. It shows `phone setup`, the six-digit setup code as two rows of
three 24pt digits (six in one row are ~150px, wider than the panel allows),
and `enter code on phone`. The code never travels over Bluetooth. The user
types it into the companion app, which proves it; only then does the unit
save the phone and hand it a random 128-bit key (PROTOCOL.md §3).

The normal screen returns after proof, disconnect, USB removal, or the
three-minute expiration. An image retained after a power cut contains an
expired code. See protocol v6 in `src/protocol/PROTOCOL.md`.

### Unavailable

`readings` / `unavailable` in 12 pt with `check sensors` in 9 pt, centred.
Shown only in place of the face when there is nothing to judge.

---

## 5. Refresh policy

E-ink refreshes are slow, visible and cost energy, so the firmware draws only
when the picture would change.

- **Skip when unchanged.** `UiModel` holds exactly what a screen draws, already
  rounded. If the new model equals the last drawn one, the panel is neither
  initialised nor refreshed; it keeps its image unpowered. Comparison covers only what the current screen shows: a change in
  light does not redraw the face. The judge mode counts where it changes the
  picture: the face's nudge words and the ledger's bands.
- **Partial refresh by default**, with a **full refresh every
  `fullRefreshEveryCycles` draws** (default 10, BLE-configurable up to 1000) to
  clear ghosting. The first draw after boot is always full.
- A draw that fails (BUSY timeout) does not update the stored model, so the
  next cycle tries again.

---

## 6. Implementation

| Module | Role |
|---|---|
| `src/ui/ComfortEvaluation` | comfort bands and severity, by judge mode — pure C++, host-tested |
| `src/ui/SleepSchedule` | day or sleep mode from the sleep window and the clock, ported from the app — pure C++, host-tested for parity |
| `src/ui/UiModel` | rounds readings, judges severities, picks the screen, compares models — pure C++, host-tested |
| `src/ui/Renderer` | draws the six layouts from a `UiModel`; never reads sensors or BLE |
| `src/drivers/EpaperDisplay` | panel lifecycle, frame buffer, fonts, full/partial refresh; the only code that sees GxEPD2 |
| `src/ui/fonts/` | Fredoka bitmaps, generated by `scripts/convert-fonts.sh` |

The renderer draws into a plain frame buffer through `EpaperDisplay`, so
GxEPD2 can be replaced without touching any layout (see the display licence
question in `SOFTWARE.md`).
