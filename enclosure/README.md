# Enclosure

3D printable enclosure designs for the Thermux temperature monitor.

There are two separate enclosures:

- **ESP32-POE-ISO Case** - the main enclosure. Houses the Olimex ESP32-POE-ISO with the **Sensor Bus Hat** plugged onto it (the Sensor Bus Hat has no enclosure of its own).
- **Temperature Node** - a two-part design (PCB + case) that houses a distributed DS18B20 sensor node along the 1-Wire bus.

## Directory Structure

```
enclosure/
├── fusion360/      # Autodesk Fusion 360 source files (.f3d / .f3z)
├── step/           # Universal CAD interchange format (.step)
├── print/          # Print-ready files (.3mf, .stl)
└── README.md
```

## Files

### ESP32-POE-ISO Case (main enclosure)

Houses the Olimex ESP32-POE-ISO with the Sensor Bus Hat plugged onto it.

- `fusion360/Thermux ESP32-POE-ISO Case.f3z` - Fusion 360 source (archive)
- `step/Thermux ESP32-POE-ISO Case.step` - STEP
- `print/ESP32-POE-ISO Case - Base.3mf` - print-ready base
- `print/ESP32-POE-ISO Case - Lid.3mf` - print-ready lid

### Temperature Node (DS18B20 sensor node)

- `fusion360/Thermux Temperature Node.f3d` - Fusion 360 source
- `step/Thermux Temperature Node.step` - STEP
- `print/Base.3mf` - print-ready base
- `print/Lid.3mf` - print-ready lid

## File Formats

- **Fusion 360 (.f3d / .f3z)** - Full design history for parametric editing (requires Fusion 360). `.f3z` is a Fusion archive export.
- **STEP (.step)** - Universal CAD format for other software (FreeCAD, SolidWorks, OnShape, etc.)
- **3MF (.3mf)** - Modern print format with colors/materials/settings (preferred)
- **STL (.stl)** - Legacy mesh format for older slicers

## Printing Recommendations

- **Material**: PETG or ASA for outdoor/moisture resistance
- **Layer Height**: 0.2mm
- **Infill**: 20-30%
- **Supports**: As needed

## License

Enclosure designs are released under [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
