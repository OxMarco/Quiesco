# Quiesco v1 — Third-party notices

The Quiesco firmware is free software under the **GNU General Public License
version 3** (`LICENSE`, SPDX `GPL-3.0-only`). Its own source files say so in
their first line. The components below keep their own licences, all
compatible with distributing the combined firmware under GPL-3.0.

Every third-party component in the Quiesco v1 firmware image, with the
version the build pins (`dependencies.lock`), where it comes from, its
licence, and any local change. The linked set was taken from a real build
(`compile_commands.json` of `make build`), not from what happens to be
installed.

`scripts/collect-licences.sh` copies each component's licence text into
`.build/licences/`, together with this file, for a release package. It also
fails if an installed version differs from the lock, so the texts always
belong to the code that was built.

## Components linked into the firmware

| Component | Version | Source | Licence | Local changes |
|---|---|---|---|---|
| Seeeduino mbed core (ArduinoCore-mbed, ArduinoCore-API) | 2.9.3 | [Seeed-Studio/ArduinoCore-mbed](https://github.com/Seeed-Studio/ArduinoCore-mbed), from [arduino/ArduinoCore-mbed](https://github.com/arduino/ArduinoCore-mbed) | LGPL-2.1 | none |
| Mbed OS (bundled in the core, precompiled) | as shipped in core 2.9.3 | [ARMmbed/mbed-os](https://github.com/ARMmbed/mbed-os) | Apache-2.0, with components under their own permissive licences (Nordic nrfx: BSD-3-Clause; Arm Cordio BLE stack: Apache-2.0; CMSIS: Apache-2.0; Mbed TLS 2.25.0, called directly for HMAC-SHA-256: Apache-2.0) | none |
| `SPI`, `Wire` (core libraries) | core 2.9.3 | core | LGPL-2.1 | none |
| Adafruit BME280 Library | 2.3.0 | [adafruit/Adafruit_BME280_Library](https://github.com/adafruit/Adafruit_BME280_Library) | BSD-3-Clause | none |
| Adafruit VEML7700 Library | 2.1.6 | [adafruit/Adafruit_VEML7700](https://github.com/adafruit/Adafruit_VEML7700) | BSD-3-Clause | none |
| Adafruit BusIO | 1.17.4 | [adafruit/Adafruit_BusIO](https://github.com/adafruit/Adafruit_BusIO) | MIT | none |
| Adafruit Unified Sensor | 1.1.15 | [adafruit/Adafruit_Sensor](https://github.com/adafruit/Adafruit_Sensor) | Apache-2.0 | none |
| Adafruit GFX Library | 1.12.6 | [adafruit/Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) | BSD-2-Clause | none |
| Sensirion I2C SCD4x | 1.1.0 | [Sensirion/arduino-i2c-scd4x](https://github.com/Sensirion/arduino-i2c-scd4x) | BSD-3-Clause | none |
| Sensirion Core | 0.7.3 | [Sensirion/arduino-core](https://github.com/Sensirion/arduino-core) | BSD-3-Clause | none |
| GxEPD2 | 1.6.9 | [ZinggJM/GxEPD2](https://github.com/ZinggJM/GxEPD2) | **GPL-3.0** | none |
| ArduinoBLE | 2.1.0 | [arduino-libraries/ArduinoBLE](https://github.com/arduino-libraries/ArduinoBLE) | **LGPL-2.1** | none |
| FlashDB (vendored in `src/third_party/flashdb`) | 2.2.0 | [armink/FlashDB](https://github.com/armink/FlashDB) | Apache-2.0 | yes: `fdb_tsdb.c` initialises an erased TSDB one sector at a time (see `README.quiesco.md`); `fdb_cfg.h` and `fal_cfg.h` are Quiesco configuration |
| Fredoka font, as bitmaps in `src/ui/fonts` | Google Fonts `ofl/fredoka` (variable font) | [google/fonts](https://github.com/google/fonts/tree/main/ofl/fredoka) | SIL OFL 1.1, © 2016 The Fredoka Project Authors | converted to Adafruit GFX bitmaps by `scripts/convert-fonts.sh`; no reserved font name |

Not linked:

- **Host test fakes** in `tests/host/fakes` are Quiesco code that only
  mimics the Arduino and ArduinoBLE interfaces.
- **Unity**, previously pinned as a development dependency, was never used by
  the host tests and has been removed from the lock and the install script.
- **Build tools**: arduino-cli, the GCC ARM toolchain, Adafruit
  `fontconvert` and fonttools. They are used to build, not shipped.

## What each licence asks of a release

This is an engineering summary to act on, not legal advice.

The firmware is released under GPL-3.0 with its complete source, which covers
the heaviest obligations below: GxEPD2's (the combined work is GPL-3.0) and
the LGPL relinking requirement, since anyone can rebuild and relink from
source. What remains for every binary release (a firmware file, or a unit
with firmware on it):

- offer the corresponding source for that exact version (tag, or archive the
  source tree with `dependencies.lock`), as GPL-3.0 §6 requires;
- ship the licence texts and notices: `make licences` plus the core's texts
  (item 3 below);
- keep the FlashDB change notice and the OFL with the fonts.

For reference, what each licence would ask of a binary-only release:

| Licence | Components | Obligation for a binary-only firmware release |
|---|---|---|
| MIT, BSD-2/3-Clause, SIL OFL 1.1 | Adafruit libraries, Sensirion libraries, Fredoka | Reproduce the copyright notices and licence texts in documentation or another place the user can find them, for example the app's "open-source licences" screen or the product manual. The BSD-3 texts also forbid using the authors' names to endorse the product. |
| Apache-2.0 | FlashDB, Adafruit Unified Sensor, Mbed OS | Ship the licence text and any upstream NOTICE file; state changes in modified files, which `fdb_tsdb.c` now does in its header. |
| LGPL-2.1 | ArduinoBLE, the core, `SPI`, `Wire` | Statically linked into one image, so the recipient must be able to relink the firmware with a modified version of the library. In practice, ship the LGPL text, offer the library source, and provide the application's object files (or its source) with instructions to relink and flash. |
| GPL-3.0 | GxEPD2 | Linking GxEPD2 makes the distributed firmware a combined work under GPL-3.0: the complete corresponding source of the whole firmware must be offered under GPL-3.0. |

## Open items before release

1. **GxEPD2 (GPL-3.0)**: resolved by releasing the firmware under GPL-3.0.
2. **LGPL-2.1 relinking** for ArduinoBLE and the core: resolved by the source
   release.
3. **Core licence texts.** The installed Seeeduino mbed 2.9.3 package ships
   no licence files, so `collect-licences.sh` cannot gather them. Take the
   LGPL-2.1 text and Mbed OS's `LICENSE.md` (with its per-component
   licences) from the upstream repositories at the matching version, and add
   them to the release package.
4. **Where the notices are shown.** Product to decide: the app's licence
   screen, the manual, or both.
