# Hardware

Open-source PCB design files for Thermux. There are **two** boards:

- **Sensor Bus Hat** ([`sensor_bus_hat/`](sensor_bus_hat/)) — plugs onto the
  Olimex ESP32-POE-ISO and breaks the 1-Wire bus out to an RJ45 jack, carrying
  the bus pull-up resistor.
- **Temperature Node** ([`temp_node/`](temp_node/)) — the distributed DS18B20
  sensor breakout with RJ45 connectors for daisy-chaining sensors along the bus.

## Directory Structure

```
hardware/
├── sensor_bus_hat/     # ESP32-POE-ISO bus-adapter hat
│   ├── kicad/          # KiCad project (.kicad_pro, .kicad_sch, .kicad_pcb)
│   └── gerbers/        # Manufacturing files (gerbers, drills, BOM, CPL)
├── temp_node/          # DS18B20 sensor breakout node
│   ├── kicad/
│   └── gerbers/
└── README.md
```

Each board's `gerbers/` folder is self-contained — zip its contents (or upload
the folder) to JLCPCB / PCBWay to fabricate that board.

## Sensor Bus Hat

Adapter board that sits on the Olimex ESP32-POE-ISO and exposes the 1-Wire bus
on an RJ45 jack. It carries the bus **pull-up resistor** (see the pull-up note
below).

### Bill of Materials

| Qty | Designator | Description | Part Number |
|-----|-----------|-------------|-------------|
| 1 | R1 | Pull-up resistor, 4.7 kΩ (0805) — see pull-up note | (generic) |
| 1 | J1 | RJ45 Connector | GCT MJ3225-88-0 |
| 1 | J2 | 10-pin 2.54 mm pin socket (to ESP32-POE-ISO) | (generic) |

> **Pull-up note:** R1 is populated as **4.7 kΩ**, which is correct for a small
> bus. For **10+ sensors** on a long bus, drop to **2.2 kΩ or 1.5 kΩ** (a
> *lower* resistance = stronger pull-up) for reliable 1-Wire communication — the
> ESP32's internal pull-up alone is too weak.

## Temperature Node

Custom breakout board with RJ45 connectors for daisy-chaining DS18B20 sensors.

### PCB Features

- DS18B20 sensor breakout
- Screw terminal connections
- Compatible with Olimex ESP32-POE-ISO

### Bill of Materials

| Qty | Description | Part Number | DigiKey Link |
|-----|-------------|-------------|--------------|
| 2 | RJ45 Connector | GCT MJ3225-88-0 | [MJ3225-88-0](https://www.digikey.com/en/products/detail/gct/MJ3225-88-0/16893750) |
| 1 | JST XH Header (3-pin) | JST S3B-XH-A | [S3B-XH-A](https://www.digikey.com/en/products/detail/jst-sales-america-inc/S3B-XH-A/1651048) |
| 1 | PTC Resettable Fuse | Murata PRG21AR420MS1RA | [PRG21AR420MS1RA](https://www.digikey.com/en/products/detail/murata-electronics/PRG21AR420MS1RA/2595394) |
| 1 | 10µF Capacitor (0805) | Samsung CL21B106KOQNNNE | [CL21B106KOQNNNE](https://www.digikey.com/en/products/detail/samsung-electro-mechanics/CL21B106KOQNNNE/3888530) |
| 1 | 0.1µF Capacitor (0805) | Samsung CL21B104KBCNNNC | [CL21B104KBCNNNC](https://www.digikey.com/en/products/detail/samsung-electro-mechanics/CL21B104KBCNNNC/3886661) |

## Manufacturing

Gerber files are ready for fabrication at JLCPCB, PCBWay, or similar services.

## License

Hardware designs are released under [CERN-OHL-P v2](https://ohwr.org/cern_ohl_p_v2.txt) (Permissive).
