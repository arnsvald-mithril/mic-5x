# MIC-5X — Indoor Microclimate Monitor

**MITHRIL ELECTRONICS** · Doc. MTH-DS-0001 · Datasheet Rev. C.1

A 5-parameter indoor microclimate monitor built on a bare ESP8266 (ESP-12F)
module with a custom-designed analog dust-sensing front-end — not a
pre-built sensor module, but a discrete nephelometric channel (IR LED +
photodiode + LM358 transimpedance amplifier) designed, simulated, and
laid out from scratch.

![Board render](docs/images/board_top.png)
*(replace with actual board photo/render)*

## Measured parameters

| Parameter          | Sensor  | Interface |
|---------------------|---------|-----------|
| Temperature          | DHT22   | Digital (1-Wire-like) |
| Relative humidity     | DHT22   | Digital |
| Barometric pressure    | BMP280  | I2C |
| Illuminance          | BH1750  | I2C |
| Particulate matter (dust) | Custom analog nephelometric channel | ADC (ESP8266 TOUT) |

Full electrical characteristics, error budget, and mechanical drawings:
see `datasheet/MIC-5X_Datasheet_RevC1.pdf`.

## Engineering highlights

- **Analog front-end design.** Custom TIA stage (LM358) for the dust
  channel, simulated in LTspice: DC transfer characteristic
  0.3-3.3 V at 0-30 uA photocurrent, two-pole filter (tau ~= 1.8 ms,
  -3 dB at ~90 Hz). See `hardware/simulation/`.
- **Full error budget.** Aggregated RSS error ~5-7%, dominated by LED
  thermal drift (~4-6%), with contributions from Vref/AMS1117 (~1.5%),
  R3 tempco (~0.3%), and LM358 bias current (~0.5%).
- **Bare-module hardware design.** Migrated from a NodeMCU dev board
  to a bare ESP-12F module with full boot-strapping (EN/RST/GPIO0/GPIO2
  pull-ups, GPIO15 pull-down), a complete power protection chain
  (PolyFuse -> Schottky -> TVS -> AMS1117), and ADC input protection
  for the dust channel.
- **DRC-clean 2-layer PCB** in KiCad, including ground pour with
  stitching vias.
- **Non-blocking firmware.** ~300-line Arduino sketch using millis()
  scheduling (no delay()), with sensor cross-validation (DHT22 vs
  BMP280 temperature check), pressure trend detection, and a fallback
  AP mode for field configuration.
- **3D-printed enclosure**, parametrically modeled in CadQuery, with
  snap-fit assembly (no screws) and a dedicated cross-flow air path
  for the dust sensor.

## Repository structure

```
mic-5x/
    hardware/
        schematic/     KiCad schematic + PDF export
        pcb/            KiCad PCB layout
        simulation/     LTspice results (TIA transfer & frequency response)
    firmware/           ESP8266 Arduino firmware
    enclosure/           CadQuery enclosure script + STEP/STL exports
    datasheet/           Full Rev. C.1 datasheet (PDF)
    docs/images/         Board photos, renders, simulation plots
```

## Known limitations

This is an honest engineering datasheet, not a marketing sheet —
current known issues:

- Dust channel is **not yet calibrated on real aerosol**; the
  0.3-3.3 V range is the simulated full-scale transfer characteristic,
  not a validated ppm/ug*m^-3 curve.
- Enclosure snap-fit tongues are sized for **PLA** specifically
  (~0.9% strain after redesign); other materials would need
  re-verification of engagement geometry.
- LTspice .asc source files were iterated in a single working draft
  and not preserved as separate versions — simulation plots are
  archived in docs/images/, and the circuit is fully reproducible
  from the parameters listed in the datasheet.
- Enclosure errata (S6.6 of the datasheet) documents six issues found
  during design review and their fixes (exhaust port, fan bore
  clearance, snap-fit strain, screw sizing, stray light path).

## License

MIT — see LICENSE.

## Author

Oleksandr Didyk — Bachelor of Electronics, Igor Sikorsky Kyiv
Polytechnic Institute, Faculty of Electronics, specialization in
Electronic Devices and Systems.
