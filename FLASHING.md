# Flashing Thermux Firmware

This guide explains how to flash the Thermux firmware to your ESP32-POE device.

## Overview

- **Initial Flash** (brand new device): Requires all three binary files (bootloader, partition table, and application)
- **OTA Updates** (existing installation): Only requires the `thermux.bin` file via web interface

## Initial Flash (Brand New Device)

When flashing a brand new ESP32-POE device that has never had Thermux installed, you need to flash three binary files to specific memory locations.

### Required Files

Download all three files from the [latest release](https://github.com/sslivins/thermux/releases/latest):

1. `bootloader.bin` - ESP32 bootloader (flashed at 0x1000)
2. `partition-table.bin` - Partition layout (flashed at 0x8000)
3. `thermux.bin` - Main application (flashed at 0x20000)

### Prerequisites

#### Install esptool.py

```bash
pip install esptool
```

Or use the version that comes with ESP-IDF if you have it installed.

#### Identify Your Serial Port

**Linux:**
```bash
ls /dev/ttyUSB*
# Usually /dev/ttyUSB0
```

**macOS:**
```bash
ls /dev/cu.usbserial-*
# e.g., /dev/cu.usbserial-0001
```

**Windows:**
- Check Device Manager → Ports (COM & LPT)
- Usually COM3, COM4, etc.

### Flashing Commands

#### Linux / macOS

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash \
  0x1000 bootloader.bin \
  0x8000 partition-table.bin \
  0x20000 thermux.bin
```

Replace `/dev/ttyUSB0` with your actual port.

#### Windows

```bash
esptool.py --chip esp32 --port COM3 --baud 460800 write_flash ^
  0x1000 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x20000 thermux.bin
```

Replace `COM3` with your actual port.

#### Using ESP-IDF

If you have ESP-IDF installed, you can also use:

```bash
idf.py -p /dev/ttyUSB0 flash
```

However, this requires the full source code and build environment.

### Verification

After flashing:

1. Open serial monitor to see boot logs:
   ```bash
   esptool.py --port /dev/ttyUSB0 monitor
   # Or
   idf.py -p /dev/ttyUSB0 monitor
   ```

2. Connect to the device:
   - Via Ethernet: Check your router's DHCP table or try `http://thermux.local`
   - Via serial console: Watch for the IP address in boot logs

3. Access the web interface at `http://thermux.local` (or the IP address shown)

## OTA Updates (Existing Installation)

Once Thermux is already running on your device, you can update easily through the web interface. **No manual file downloads are needed** for automatic updates - the web interface handles everything for you.

### Option 1: Automatic Updates (Recommended)

The easiest way to update is to let the web interface download and install updates automatically from GitHub releases.

1. Go to `http://thermux.local/config`
2. In the OTA section, set the **OTA URL** to:
   ```
   https://api.github.com/repos/sslivins/thermux/releases/latest
   ```
3. Click "Save Configuration"
4. Go to `http://thermux.local/ota`
5. Click "Check for Updates"
6. If a newer version is available, click "Update Now"
7. **The device downloads the firmware automatically**, flashes it, and reboots - you don't need to download `thermux.bin` yourself

> **Note**: This is the recommended method for most users. The web interface downloads the latest `thermux.bin` from GitHub releases automatically.

### Option 2: Manual File Upload

Use this option if you want to install a **specific version** or **older version** of the firmware.

1. Download the desired `thermux.bin` from any [release](https://github.com/sslivins/thermux/releases)
2. Navigate to `http://thermux.local/ota`
3. Click "Choose File" and select the `thermux.bin` file you downloaded
4. Click "Upload & Flash"
5. Wait for the upload and flash to complete (~30 seconds)
6. The device will automatically reboot with the selected firmware

> **Use case**: Installing a specific version (e.g., rolling back to an older version or testing a pre-release)

### Option 3: Via Home Assistant

You can trigger OTA updates from Home Assistant using a REST command:

```yaml
# configuration.yaml
rest_command:
  update_thermux:
    url: "http://thermux.local/api/ota/update"
    method: POST
```

Then create an automation or script to call `rest_command.update_thermux`.

## Troubleshooting

### Flash Fails with "Failed to connect"

1. **Check USB cable**: Use a data cable, not a charge-only cable
2. **Hold BOOT button**: Some ESP32 boards require holding BOOT while connecting
3. **Try lower baud rate**: Use `--baud 115200` instead of 460800
4. **Check drivers**: Install CH340/CP2102 USB-to-serial drivers if needed

### Device Doesn't Boot After Flash

1. **Verify all three files were flashed** at the correct addresses
2. **Check file versions match**: All three files should be from the same release
3. **Try erasing flash first**:
   ```bash
   esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash
   ```
   Then flash again

### Can't Access Web Interface

1. **Check Ethernet connection**: Ensure cable is connected and link lights are on
2. **Check serial console**: Look for IP address in boot logs
3. **Try IP address directly**: If mDNS isn't working, use the IP from serial console
4. **Check router**: Look for "thermux" in DHCP client list
5. **Multiple devices**: If you have multiple Thermux devices, they'll be numbered (thermux-2.local, thermux-3.local, etc.)

### OTA Update Fails

1. **Check available flash space**: Monitor logs during update
2. **Verify network connection**: Ensure stable WiFi/Ethernet
3. **Check file size**: thermux.bin should be ~900KB - 1.2MB
4. **Try manual upload**: Use the web interface upload method instead of automatic
5. **Check logs**: View logs at `http://thermux.local/config` → Log Viewer section

## Advanced: Building from Source

If you want to build and flash from source code:

### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Git (for cloning repository)

### Build & Flash

```bash
# Clone repository
git clone https://github.com/sslivins/thermux.git
cd thermux

# Set ESP32 target
idf.py set-target esp32

# Build firmware
idf.py build

# Flash all partitions and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

The `idf.py flash` command automatically flashes all required partitions (bootloader, partition table, and application) to the correct addresses.

### Just Build (No Flash)

```bash
idf.py build
```

Binary files will be in `build/`:
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/thermux.bin`

## Flash Memory Layout

The Thermux firmware uses the following partition layout (as of v2.10.13 and later):

| Partition | Type | Address | Size | Purpose |
|-----------|------|---------|------|---------|
| Bootloader | - | 0x1000 | ~28KB | ESP32 second-stage bootloader |
| Partition Table | - | 0x8000 | ~3KB | Partition layout definition |
| NVS | data | 0x9000 | 24KB | Configuration storage (WiFi, MQTT, etc.) |
| OTA Data | data | 0xf000 | 8KB | OTA update state |
| phy_init | data | 0x11000 | 4KB | WiFi PHY calibration data |
| OTA_0 | app | 0x20000 | 1.875MB | OTA update slot 0 |
| OTA_1 | app | 0x200000 | 1.875MB | OTA update slot 1 |
| Storage | data | 0x3F0000 | 64KB | Additional NVS storage |

A brand-new device has no factory partition; the bootloader defaults to booting `OTA_0` when `otadata` is blank (all-0xFF), so an initial flash of `thermux.bin` at `0x20000` boots normally on first power-up. Once OTA updates begin, the device boots from whichever of `OTA_0`/`OTA_1` has the newest firmware.

### Partition layout history

Prior to v2.10.13, the layout used a separate `factory` partition (0x20000, 1.4MB) plus two smaller 1.2MB OTA slots (`OTA_0` at 0x190000, `OTA_1` at 0x2C0000). The `factory` slot was only ever used for the initial flash and was permanently wasted space afterward (a device never boots back into `factory` once an OTA update lands in `OTA_0`/`OTA_1`).

Existing field devices flashed before this change were migrated to the new layout entirely over-the-air (no reflash required), reclaiming the dead `factory` space and growing both OTA slots to 1.875MB each. See [issue #48](https://github.com/sslivins/thermux/issues/48) for the full migration design and validation history. This was a one-time, in-place migration; it does not need to be repeated and does not apply to devices already running v2.10.13+.

## Security Notes

- **Flash encryption**: Not enabled by default. Can be configured via `idf.py menuconfig`
- **Secure boot**: Not enabled by default. Can be configured via `idf.py menuconfig`
- **API authentication**: Disabled by default. Enable in web interface → Settings → Security

For production deployments requiring flash encryption or secure boot, refer to the [ESP-IDF Security Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/index.html).

## Support

- **GitHub Issues**: [https://github.com/sslivins/thermux/issues](https://github.com/sslivins/thermux/issues)
- **Discussions**: [https://github.com/sslivins/thermux/discussions](https://github.com/sslivins/thermux/discussions)
- **Documentation**: See [README.md](README.md) for feature documentation
