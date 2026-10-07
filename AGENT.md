# Updating the Quiesco firmware

How to put new firmware on a Quiesco unit. Written for an agent helping a
user, and readable by the user. Owners need **no software** beyond the
companion app: the update is a file copy onto a USB drive, the same on
macOS, Windows and Linux.

Background: the unit is a Seeed XIAO nRF52840 Plus. Its bootloader can show
itself as a USB drive called `XIAO-BOOT` and flashes any firmware `.uf2`
copied onto it. Settings, enrolled phones, calibration and the measurement
log live in the unit's external flash, which an update never touches.

## What you need

- The new firmware file, `quiesco-X.Y.Z.uf2`, from the release (see
  [Making a release](#making-a-release)).
- A computer with a USB port, and a USB-C cable that carries data (some
  charge-only cables do not).
- The companion app, connected to the unit. Its **Unit → Update firmware**
  row appears only when the unit's firmware supports it (capability bit 16,
  `firmware/src/protocol/PROTOCOL.md` §9.2). Without it, see
  [Without the app](#without-the-app).

## Update steps

1. Plug the unit into the computer with the USB-C cable.
2. In the app, open **Unit**, tap **Update firmware**, then **Continue**.
   Within about 15 seconds the unit disconnects from the app and restarts as
   a USB drive called `XIAO-BOOT`. It stops measuring until step 4 is done.
   If the app says to plug the unit in, it did not see USB power: check the
   cable.
3. Wait until the computer shows the `XIAO-BOOT` drive:
   - **macOS:** it appears on the desktop and in Finder's sidebar.
   - **Windows:** it appears in File Explorer under This PC, with a drive
     letter.
   - **Linux:** desktops mount it automatically (Files, Dolphin, and so on).
     Without a desktop: `lsblk` to find it, then `udisksctl mount -b /dev/sdX`.
4. Copy `quiesco-X.Y.Z.uf2` onto the drive (drag and drop, or copy and
   paste). The drive disappears by itself within a few seconds: that is the
   unit restarting with the new firmware.
5. In the app, connect again: **Unit → Firmware** shows the new version.

The computer may complain that the drive was not ejected properly, or (on
macOS) that the copy could not finish. Both are harmless: the unit restarts
as soon as the last block is written, before the computer has tidied up.
From a terminal on macOS, `cp -X quiesco-X.Y.Z.uf2 /Volumes/XIAO-BOOT/` avoids
the message. If a copy is refused with "permission denied", the drive was
still mounting: wait two seconds and copy again.

### If something goes wrong

- **The drive stays and nothing happens.** The file was not a valid
  Quiesco `.uf2` (for example a `.zip`, `.hex` or `.bin`). Copy the right
  file; nothing has been changed yet.
- **The cable was unplugged before the copy.** The unit waits as a drive,
  without measuring, even on battery. Plug it back in and copy the file (the
  same version it had is fine).
- **The unit does not come back after the copy.** Unplug and replug the
  cable. If `XIAO-BOOT` appears again, copy the file again.
- **The app no longer shows the unit.** On iPhone, an update can leave iOS
  with a stale view of the unit: forget "Quiesco" under Settings →
  Bluetooth, then connect from the app again. The unit keeps its enrolled
  phones, so no new setup code is needed.

## Without the app

For units whose firmware predates the **Update firmware** row, or when no
phone is at hand, the drive can be reached with the XIAO's reset button.
It sits inside the case:

1. Remove the four screws and open the case (`hardware/README.md`,
   assembly).
2. Plug the unit into the computer.
3. Press the XIAO's tiny reset button twice, quickly. `XIAO-BOOT` appears.
4. Continue from step 4 above, then close the case.

A developer with the bench tools can trigger the drive over Bluetooth
instead, from macOS or Linux (needs Python and `pip install bleak`, and an
enrolled key, see `firmware/scripts/ble-test.py`):

```sh
firmware/scripts/ble-test.py usb-update
```

## Making a release

Building needs the Arduino toolchain: macOS or Linux with `bash`, `make`,
`python3` and `arduino-cli` (`firmware/README.md`, Quick start; the first
install downloads the board core and compiler, a few hundred MB). On
Windows, use WSL; the build scripts are bash. Owners never build.

1. Set the version in `firmware/VERSION` (it also goes into the image).
2. `cd firmware && make verify && make build`.
3. The build writes `.build/quiesco_firmware/quiesco-X.Y.Z.uf2` (made by
   `scripts/make-uf2.py` from the image) next to the `.hex` and `.zip`.
4. Publish the `.uf2` with the release notes (a GitHub release on
   `OxMarco/Quiesco`; the repository is private for now, so owners cannot
   download from it yet).

Always ship a **release** build (`make build`). Never give owners a debug
build (`--debug`) or a bench build (`--no-battery`).

## For agents: rules

- Use only the steps above. Do not use `scripts/ble-test.py update` (BLE
  over-the-air update): it is disabled in shipped firmware because the
  bootloader never finishes the erase, which leaves the unit needing USB
  (`PROTOCOL.md` §9.3).
- Never send a factory reset or a log erase as part of an update. Neither
  is needed: an update keeps settings, phones and history.
- A developer with the repository can also flash over USB with
  `arduino-cli` (`firmware/README.md`, Build and flash). That is for
  development, not for owners.
- Check the result: the firmware version in the app, or DIS `2A26` /
  `scripts/ble-test.py info`.
