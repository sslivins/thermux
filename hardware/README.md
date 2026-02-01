# Hardware

PCB design files for the Thermux temperature sensor breakout board.

## Directory Structure

```
hardware/
├── kicad/          # KiCad project files (.kicad_pro, .kicad_sch, .kicad_pcb)
├── gerbers/        # Manufacturing files for PCB fabrication
└── README.md
```

## PCB Features

- DS18B20 sensor breakout
- Screw terminal connections
- Compatible with Olimex ESP32-POE-ISO

## Bill of Materials

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
