# Quiesco: compliance pack for the CE test lab

This folder answers the test lab's questions about the radio module in
Quiesco v1 (PCB `L-1K1W1007851A`, firmware `1.0.0`). Every PDF in it was
downloaded unchanged from Seeed Studio's official site
(`files.seeedstudio.com`) on 2026-10-10.

| # | The lab asked for | Where it is |
|---|---|---|
| 1 | Full datasheet for the Seeed Studio XIAO nRF52840 Plus | [`xiao-nrf52840-plus/datasheets/`](xiao-nrf52840-plus/datasheets/) |
| 2 | RF test reports and other compliance documents for the module | [`xiao-nrf52840-plus/certificates/`](xiao-nrf52840-plus/certificates/) and [the FCC filing](#2-rf-test-reports-and-compliance-documents) |
| 3 | Confirmation that the module, antenna and RF settings are unchanged | [Section 3](#3-the-module-is-used-unmodified) |
| 4 | EMC or electrical safety reports for the complete product | [Section 4](#4-emc-and-safety-reports-for-the-complete-product): none yet |

## The radio module in one table

| | |
|---|---|
| Module | Seeed Studio XIAO nRF52840 Plus, Seeed SKU **102010672**, model `XIAO nRF52840-Plus` |
| Module manufacturer | Seeed Technology Co., Ltd., Shenzhen, China |
| Chipset | Nordic Semiconductor nRF52840 |
| Radio | Bluetooth Low Energy only, 2402–2480 MHz, GFSK |
| Antenna | integrated ceramic chip antenna on the module, 2.09 dBi max gain |
| Max output power (CE annex) | 8.27 dBm |
| EU RED certificate | **AT1812C501020125**, Shenzhen Anbotek Compliance Laboratory, 28 Apr 2025 |
| FCC ID | **Z4T-XIAO52840P** (single modular approval, 27 Apr 2025) |
| Japan | **R 222-257139** (Derycom, 25 Apr 2025) |

## 1. Datasheets

In [`xiao-nrf52840-plus/datasheets/`](xiao-nrf52840-plus/datasheets/):

| File | What it is |
|---|---|
| `XIAO-nRF52840-Plus-product-sheet-102010672.pdf` | Seeed's datasheet for this exact SKU (the Plus) |
| `XIAO-nRF52840-Plus-schematic-v1.0.pdf` | module schematic and PCB, from Seeed's `SCH_PCB_v1.1` package |
| `Seeed-Studio-XIAO-Series-SOM-Datasheet.pdf` | Seeed's datasheet for the XIAO family as a system-on-module |
| `Nordic-nRF52840-Product-Specification-v1.5.pdf` | the SoC, including the radio's electrical specification |
| `BQ25101-charger.pdf` | the module's on-board Li-ion charger IC |
| `XIAO-nRF52840-Sense-BLE-range-test-report.pdf` | Seeed's BLE range test (Sense variant, same radio and antenna) |

## 2. RF test reports and compliance documents

In [`xiao-nrf52840-plus/certificates/`](xiao-nrf52840-plus/certificates/), as
published by Seeed for SKU 102010672:

| File | Scope | Issued by |
|---|---|---|
| `102010672-CE.pdf` | RED 2014/53/EU certificate of conformity, art. 3(1)(a), 3(1)(b), 3(2) | Shenzhen Anbotek Compliance Laboratory |
| `102010672-UKDOC.pdf` | UK Declaration of Conformity (Radio Equipment Regulations 2017, RoHS 2012) | Seeed Technology |
| `102010672-ROHS.pdf` | RoHS 2011/65/EU & (EU) 2015/863 certificate | Shenzhen Anbotek Compliance Laboratory |
| `102010672-FCC.pdf` | FCC Part 15C grant, single modular approval | Derycom Certification Services (TCB) |
| `102010672-TELEC.pdf` | Japan certificate of construction type | Derycom Certification Services |

**Standards covered by the module's RED certificate**

| RED article | Standard | Anbotek report no. |
|---|---|---|
| 3(1)(a) safety | EN IEC 62368-1:2020+A11:2020 | 1812C50101422101 |
| 3(1)(a) health | EN 50663:2017, EN 62479:2010 | 1812C50102012502H |
| 3(1)(b) EMC | ETSI EN 301 489-1 V2.2.3, ETSI EN 301 489-17 V3.2.4 | 1812C50102012501E |
| 3(2) radio | ETSI EN 300 328 V2.2.2 | 1812C50102012503W |

The full EU test reports listed above are not published by Seeed. The
certificate states that they are "at the applicant's disposal", so they can be
requested from Seeed Technology Co., Ltd. if the lab needs them.

**Full RF test reports in the public FCC filing.** The FCC test reports for
the same module (same radio and antenna, tested by Anbotek) are public:

- Filing overview: <https://fccid.io/Z4T-XIAO52840P>
- [Part 15C BLE test report (Anbotek)](https://fccid.io/Z4T-XIAO52840P/Test-Report/Sense-Plus-FCC-Anbotek-Test-Report-8240309)
- [BLE appendix test data](https://fccid.io/Z4T-XIAO52840P/Test-Report/FCC-BLE-Appendix-Test-Data-8240308)
- [Antenna test report](https://fccid.io/Z4T-XIAO52840P/Test-Report/Antenna-test-report-8240310)
- [RF exposure (MPE)](https://fccid.io/Z4T-XIAO52840P/RF-Exposure-Info/MPE-8240307)
- [Internal photos](https://fccid.io/Z4T-XIAO52840P/Internal-Photos/Internal-Photos-8240313)
  and [external photos](https://fccid.io/Z4T-XIAO52840P/External-Photos/External-PhotoS-8240312)
  of the module

## 3. The module is used unmodified

For Quiesco v1 we confirm that:

- **Module.** The Seeed Studio XIAO nRF52840 Plus (SKU 102010672) is bought
  from Seeed as a finished module and soldered to our carrier PCB
  (reference U3) through its castellated and bottom pads. Nothing on the
  module is added, removed, reworked or replaced.
- **Antenna.** The module's own integrated ceramic antenna is the only
  antenna. There is no external antenna, no antenna connector and no RF trace
  on our PCB.
- **Radio use.** The only radio function is Bluetooth Low Energy, using the
  nRF52840's GFSK modulation in the 2402–2480 MHz band. NFC is not used:
  the NFC pins are used as plain GPIO, and no NFC antenna is fitted.
- **RF settings.** The firmware (ArduinoBLE 2.1.0 on the Seeeduino mbed core
  2.9.3) does not change the transmit power, channel map, PHY or any other
  radio parameter, so the BLE stack defaults apply. The nRF52840 hardware tops
  out at +8 dBm, which is within the 8.27 dBm maximum in the module's RED
  certificate. The firmware has no RF test mode and no way to raise the
  output power.
- **No other transmitters.** Quiesco has no other radio, so the module is not
  co-located with any other transmitter.

### How Quiesco differs from the module as tested

These are the facts about the final product that the lab will want for its
assessment. They are not changes to the module.

- **Enclosure.** A two-part 3D-printed plastic case, about 53 × 53 × 15 mm,
  closed with four M3 screws.
- **Battery.** A 3.7 V ~400 mAh LiPo (453035 size) sits in the lid, directly
  above the XIAO module.
- **Power.** Battery-powered. Charged over the module's own USB-C port at
  5 V DC. There is no mains connection and no power supply ships with the
  product.

Name and signature of the manufacturer's authorised person:

```text
Name:       ____________________________

Company:    ____________________________

Signature:  ____________________________      Date: ______________
```

## 4. EMC and safety reports for the complete product

No EMC or electrical safety test has been run on the complete Quiesco
product yet. The tests at your lab will be the first.

To help plan the EMC test, these are the clocks and switching sources in
the product besides the radio:

| Source | Frequency |
|---|---|
| nRF52840 CPU | 64 MHz, 32 MHz crystal |
| SPI bus (flash, e-ink panel) | flash at 8 MHz |
| I²C bus (BME280, VEML7700, SCD41) | 100 kHz |
| PDM microphone clock | 1.032 MHz |
| E-ink gate-voltage boost (Q2, L1, D1–D3) | switches only during a display refresh |
| USB-C | USB 2.0 full speed, only when connected for charging or data |

The full board design is in this repository:

- Schematic: [`hardware/sources/L-1K1W1007851A.pdf`](../hardware/sources/L-1K1W1007851A.pdf)
- Bill of materials and assembly files: [`hardware/assembly/`](../hardware/assembly/)
- Enclosure: [`hardware/case/`](../hardware/case/)
- Board description: [`hardware/README.md`](../hardware/README.md) and
  [`firmware/HARDWARE.md`](../firmware/HARDWARE.md)

---

<sub>← Back to the [Quiesco overview](../README.md)</sub>
