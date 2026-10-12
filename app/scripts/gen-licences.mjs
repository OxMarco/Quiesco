#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Writes src/data/licences.json: every third-party component in the app and in
// the unit's firmware, with its licence text, for Settings → Open-source
// licences. The BSD and MIT licences ask for their notices to reach the user,
// and a unit ships no screen of its own to show them on.
//
//   cd firmware && make licences     # the firmware's texts, from what was built
//   cd app && npm run licences
//
// Run it before every release; the file is checked in, so builds and CI need
// neither the Arduino libraries nor this script.

import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const app = join(dirname(fileURLToPath(import.meta.url)), '..');
const firmwareLicences = join(app, '..', 'firmware', '.build', 'licences');
const out = join(app, 'src', 'data', 'licences.json');

const LICENCE_FILE = /^(licen[cs]e|copying|notice|ofl)/i;

/** Every licence and notice file in a folder, in a stable order, joined. */
function licenceText(dir) {
  if (!existsSync(dir)) return null;
  const files = readdirSync(dir)
    .filter((f) => LICENCE_FILE.test(f))
    .sort();
  if (files.length === 0) return null;
  return files.map((f) => readFileSync(join(dir, f), 'utf8').trim()).join('\n\n');
}

const texts = [];
const textIndex = new Map();
function textId(text) {
  if (!textIndex.has(text)) {
    textIndex.set(text, texts.length);
    texts.push(text);
  }
  return textIndex.get(text);
}

// ------------------------------------------------------------ the firmware

// As firmware/THIRD_PARTY_NOTICES.md lists them; `dir` is the folder
// collect-licences.sh writes for each.
const FIRMWARE = [
  { name: 'Seeeduino mbed core (ArduinoCore-mbed)', version: '2.9.3', licence: 'LGPL-2.1', url: 'https://github.com/Seeed-Studio/ArduinoCore-mbed', dir: 'ArduinoCore-mbed' },
  { name: 'Mbed OS, with Nordic nrfx, Arm Cordio, CMSIS and Mbed TLS', version: 'core 2.9.3', licence: 'Apache-2.0, Nordic 5-Clause', url: 'https://github.com/ARMmbed/mbed-os', dir: 'Mbed-OS' },
  { name: 'Adafruit BME280 Library', version: '2.3.0', licence: 'BSD-3-Clause', url: 'https://github.com/adafruit/Adafruit_BME280_Library', dir: 'Adafruit_BME280_Library' },
  { name: 'Adafruit VEML7700 Library', version: '2.1.6', licence: 'BSD-3-Clause', url: 'https://github.com/adafruit/Adafruit_VEML7700', dir: 'Adafruit_VEML7700_Library' },
  { name: 'Adafruit BusIO', version: '1.17.4', licence: 'MIT', url: 'https://github.com/adafruit/Adafruit_BusIO', dir: 'Adafruit_BusIO' },
  { name: 'Adafruit Unified Sensor', version: '1.1.15', licence: 'Apache-2.0', url: 'https://github.com/adafruit/Adafruit_Sensor', dir: 'Adafruit_Unified_Sensor' },
  { name: 'Adafruit GFX Library', version: '1.12.6', licence: 'BSD-2-Clause', url: 'https://github.com/adafruit/Adafruit-GFX-Library', dir: 'Adafruit_GFX_Library' },
  { name: 'Sensirion I2C SCD4x', version: '1.1.0', licence: 'BSD-3-Clause', url: 'https://github.com/Sensirion/arduino-i2c-scd4x', dir: 'Sensirion_I2C_SCD4x' },
  { name: 'Sensirion Core', version: '0.7.3', licence: 'BSD-3-Clause', url: 'https://github.com/Sensirion/arduino-core', dir: 'Sensirion_Core' },
  { name: 'GxEPD2', version: '1.6.9', licence: 'GPL-3.0', url: 'https://github.com/ZinggJM/GxEPD2', dir: 'GxEPD2' },
  { name: 'ArduinoBLE', version: '2.1.0', licence: 'LGPL-2.1', url: 'https://github.com/arduino-libraries/ArduinoBLE', dir: 'ArduinoBLE' },
  { name: 'FlashDB (modified)', version: '2.2.0', licence: 'Apache-2.0', url: 'https://github.com/armink/FlashDB', dir: 'FlashDB' },
  { name: 'Fredoka font', version: 'Google Fonts', licence: 'OFL-1.1', url: 'https://github.com/google/fonts/tree/main/ofl/fredoka', dir: 'Fredoka' },
];

if (!existsSync(firmwareLicences)) {
  console.error(`No firmware licences at ${firmwareLicences}. Run \`make licences\` in firmware/ first.`);
  process.exit(1);
}
const firmware = FIRMWARE.map(({ dir, ...c }) => {
  const text = licenceText(join(firmwareLicences, dir));
  if (!text) {
    console.error(`No licence text for ${c.name} in ${join(firmwareLicences, dir)}.`);
    process.exit(1);
  }
  return { ...c, text: textId(text) };
});

// ------------------------------------------------------------ the app

// Every production dependency, transitive ones included. Some only run at
// build time; listing them too errs on the side of giving notice.
const paths = execFileSync('npm', ['ls', '--omit=dev', '--all', '--parseable'], { cwd: app, encoding: 'utf8', maxBuffer: 64 << 20 })
  .split('\n')
  .filter((p) => p && p !== app);

const seen = new Map();
const missing = [];
for (const dir of paths) {
  const pkg = JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8'));
  const key = `${pkg.name}@${pkg.version}`;
  if (seen.has(key)) continue;
  const licence = typeof pkg.license === 'string' ? pkg.license : (pkg.license?.type ?? 'see text');
  const text = licenceText(dir);
  if (!text) missing.push(key);
  const repo = typeof pkg.repository === 'string' ? pkg.repository : pkg.repository?.url;
  seen.set(key, {
    name: pkg.name,
    version: pkg.version,
    licence,
    url: pkg.homepage ?? repo?.replace(/^git\+/, '').replace(/\.git$/, '') ?? null,
    // No file shipped: the licence named in package.json is all there is.
    text: textId(text ?? `Licensed under ${licence}, as declared by the package. No licence file is included in the published package.`),
  });
}
const appItems = [...seen.values()].sort((a, b) => a.name.localeCompare(b.name));

writeFileSync(
  out,
  `${JSON.stringify({
    sections: [
      { title: 'Firmware on the unit', items: firmware },
      { title: 'This app', items: appItems },
    ],
    texts,
  })}\n`,
);
console.log(`${firmware.length} firmware components, ${appItems.length} app packages, ${texts.length} distinct texts → ${out}`);
if (missing.length) console.log(`${missing.length} packages ship no licence file; their package.json licence is shown instead.`);
