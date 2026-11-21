# Testing a unit

How to check every assembled Quiesco before it ships, whether you build one
for yourself or a batch in a factory. You need no toolchain, terminal or
phone: you copy a file onto the unit, wait, and read the verdict on its
screen. About three minutes per unit.

- [What you need](#what-you-need)
- [Test a unit](#test-a-unit)
- [Reading the result](#reading-the-result)
- [When a unit fails](#when-a-unit-fails)
- [Notes for a batch](#notes-for-a-batch)

## What you need

- A computer with a USB port (macOS, Windows or Linux) and a USB-C data
  cable. Some cables only charge: if no drive appears in step 1, try another.
- Two files from the **same** firmware release:
  - `quiesco-factory-test-X.Y.Z.uf2`: the test
  - `quiesco-X.Y.Z.uf2`: the firmware the unit ships with

  Get both from the release page, or build them yourself
  (`make smoke` and `make build` in [`firmware/`](firmware/README.md); they
  land in `firmware/.build/smoke-sensors/` and `firmware/.build/quiesco_firmware/`).
- A fully assembled unit with the **battery plugged in** and the display
  stuck on its outline, but the **case still open**: you need to reach the
  XIAO's tiny reset button.
- The case front (the part with the window), to check the display's
  centring.

## Test a unit

1. **Load the test.** Plug the unit into the computer. Press the XIAO's reset
   button **twice, quickly**. A drive called `XIAO-BOOT` appears. Drag
   `quiesco-factory-test-X.Y.Z.uf2` onto it in Finder, File Explorer or your
   file manager. The drive disappears by itself: the unit is restarting with
   the test.

   Use the file manager rather than a terminal copy: on macOS a terminal
   `cp` onto this drive can hang. A warning that the drive was not ejected
   properly is harmless.

2. **Wait about 30 seconds**, with the unit still plugged in. The test checks
   every sensor, the battery, the memory chip, the display and the radio,
   then leaves its verdict on the screen.

3. **Read the verdict** ([below](#reading-the-result)). With the case front
   laid over the unit, check the display's centring at the same time. To run
   the test again, press reset **once**. (Unplugging does not restart it:
   the battery keeps it running.)

4. **Load the shipping firmware.** Only on a PASS: press reset twice again
   and drag `quiesco-X.Y.Z.uf2` onto `XIAO-BOOT`. Within about 15 seconds the
   screen shows the phone setup screen or a reading. That is how the unit
   ships.

5. **Close the case** and label the unit with the firmware version.

## Reading the result

| The screen shows | Verdict |
|---|---|
| **PASS**, black on white, inside a dotted border with rounded corners | Pass, if the border is complete and even all the way round |
| **FAIL** in white on a black screen, with one or more part names under it | Fail: see [When a unit fails](#when-a-unit-fails) |
| Anything else: blank, a checkerboard, an old screen, a partial image | Fail: the display did not work |

Look at the PASS screen closely:

- **The dotted border** must be complete, with no missing dots, grey patches
  or faint rows. Gaps point to a badly seated display cable.
- **Centring.** Lay the case front over the unit. The case window hides a
  couple of pixels on every side, so on a well-placed display the whole
  border shows, at an even distance from the window edge. If the border is
  cut off or crowded on one side, peel the display and re-stick it on its
  outline, then test again.

The display is the only part placed by hand, and nothing in the case lines
it up sideways. That makes this check the one most worth doing carefully.

## When a unit fails

Set the unit aside with the FAIL screen still showing, and photograph it.
The names on the screen tell you where to look:

| Name | Part | First things to check |
|---|---|---|
| `BME280` | temperature, humidity and pressure sensor (U2) | its solder joints, and the sensor power rail |
| `VEML7700` | light sensor (U5) | its solder joints, and the sensor power rail |
| `SCD41` | CO₂ sensor | its solder joints and decoupling capacitor; a supply that sags under load |
| `MIC` | microphone (U4) | its solder joints and that its sound hole is clear |
| `BATTERY` | the cell on J2 | that the battery is plugged in and charged (3.0–4.35 V) |
| `FLASH` | memory chip (U1) | its solder joints |
| `DISPLAY` | e-ink panel on J1 | that the flat cable is fully inserted and latched |
| `BLE` | the radio inside the XIAO module | reseat or replace the XIAO |

Fix it and run the test again from step 1. A unit ships only after a PASS
on its final hardware.

For a detailed diagnosis, open the unit's serial port at 115200 baud while
the test is loaded (any serial monitor, such as the Arduino IDE's) and press
`t`. The test then prints a line per part with a hint, and has extra
commands for narrowing down a fault. See
[`firmware/HARDWARE.md` §7–8](firmware/HARDWARE.md#8-unit-test-sketch--smokesensors).

## Notes for a batch

- **Keep a record** per unit: PASS or FAIL, the failed parts, and the
  firmware version loaded. With a serial monitor open, the test also prints
  the unit's serial number (`serial=` line).
- **Calibration.** Units ship with the CO₂ sensor's factory calibration.
  Nothing to do; owners can recalibrate from the app.
- **Closed units.** Once the shipping firmware is loaded and the case is
  closed, the reset button can no longer be reached. To test such a unit
  again, start a firmware update from the app (**Device → Update firmware**)
  so `XIAO-BOOT` appears, then continue from step 1, dragging the file on.
- **Owners' data is safe.** The test never touches settings, paired phones
  or the reading history, so a returned unit can be tested without wiping it.
