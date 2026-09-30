# Thermux

A multi-sensor temperature monitoring system for ESP32-POE boards with Home Assistant integration via MQTT auto-discovery. **Supports 20 DS18B20 sensors on a single 1-Wire bus by default, configurable up to 100** (`CONFIG_MAX_SENSORS`) - perfect for monitoring multiple zones, equipment, or environments from one device.

## Features

- **Up to 100 DS18B20 Sensors** - Monitor multiple temperature points from a single device on one 1-Wire bus (20 by default; raise `CONFIG_MAX_SENSORS` in menuconfig, max 100)
- **Optimized Parallel Reads** - Uses 1-Wire skip ROM command to read all sensors simultaneously (~1050ms for 20 sensors in 12-bit mode, ~450ms in 9-bit)
- **Home Assistant Integration** - MQTT auto-discovery for seamless integration
- **Modbus TCP** - Optional read-only Modbus TCP server so PLCs and heat pump controllers can read temperatures directly (see [Modbus TCP](#modbus-tcp))
- **Web Interface** - Configuration and monitoring via built-in web server
- **Sensor Identification** - Change detection highlighting helps identify which physical sensor is which
- **Custom Sensor Names** - Assign friendly names to sensors via web UI (persisted in NVS)
- **OTA Updates** - Over-the-air firmware updates from GitHub releases or manual upload with progress display, including a native Home Assistant firmware **update entity** with an Install button and progress bar
- **Ethernet & WiFi** - Primary Ethernet with WiFi fallback
- **mDNS** - Access via `thermux.local` (auto-increments on collision: thermux-2.local, etc.)
- **Service Discovery** - Discoverable via `_thermux._tcp` and `_http._tcp` services
- **Web-based Logs** - View system logs without serial connection (16KB circular buffer)
- **Bus Error Tracking** - Monitor 1-Wire CRC error rates per sensor and globally via web UI and Home Assistant
- **Runtime Log Level Control** - Change log verbosity via web UI without reflashing
- **Session-based Authentication** - Optional password protection with login page
- **API Key Authentication** - Stateless API access for scripts and automation

## Hardware Requirements

- **Board**: [Olimex ESP32-POE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/) (or compatible ESP32-POE board)
- **Sensors**: DS18B20 1-Wire temperature sensors
- **Connection**: Sensors connected to GPIO4 (configurable in menuconfig)
- **PCB** (optional): Custom breakout board - see [hardware/](hardware/) for KiCad files and BOM
- **Enclosure** (optional): 3D printable case - see [enclosure/](enclosure/) for print files

### Wiring

| DS18B20 Pin | ESP32-POE |
|-------------|-----------|
| VCC (Red)   | 3.3V      |
| GND (Black) | GND       |
| DATA (Yellow) | GPIO4   |

> **Note**: A 4.7kΩ pull-up resistor is required between DATA and VCC. For 10+ sensors, use 2.2kΩ or 1.5kΩ to ensure reliable bus communication (the ESP32's internal pull-up is too weak for 1-Wire).

## Installation

### Pre-built Firmware (Recommended)

Download the latest firmware from [GitHub Releases](https://github.com/sslivins/thermux/releases/latest) and follow the [FLASHING.md](FLASHING.md) guide.

**For brand new ESP32-POE devices**, you'll need all three files:
- `bootloader.bin`
- `partition-table.bin`
- `thermux.bin`

**For OTA updates** on devices already running Thermux, use the web interface at `http://thermux.local/ota` - it downloads and installs updates automatically (no manual file download needed). You can also manually upload a specific `thermux.bin` version if desired.

See [FLASHING.md](FLASHING.md) for detailed instructions.

### Building from Source

#### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- VS Code with ESP-IDF extension (recommended)

#### Build & Flash

```bash
# Set target
idf.py set-target esp32

# Build
idf.py build

# Flash
idf.py -p COMx flash monitor
```

Or use the ESP-IDF VS Code extension build/flash commands.

## Configuration

After flashing, access the web interface at `http://thermux.local` or the device IP.

> **Note**: If multiple devices are on the network, subsequent devices will be `thermux-2.local`, `thermux-3.local`, etc.

### Web Interface Pages

- **/** - Dashboard with live temperature readings
- **/config** - Configuration settings (MQTT, WiFi, sensor intervals, OTA)
- **/ota** - Manual firmware upload

### Configuration Options

| Setting | Description |
|---------|-------------|
| **MQTT Broker** | Home Assistant MQTT broker address |
| **MQTT Port** | Broker port (default: 1883) |
| **MQTT Username/Password** | Authentication credentials |
| **WiFi SSID/Password** | Fallback WiFi credentials |
| **Read Interval** | Sensor polling interval (seconds) |
| **Publish Interval** | MQTT publish interval (seconds) |
| **OTA URL** | GitHub releases URL for automatic updates |
| **Security** | Enable/disable password protection |

## REST API

Full API documentation is available as an OpenAPI 3.1 spec: [`docs/openapi.yaml`](docs/openapi.yaml)

View it interactively: [Swagger Editor](https://editor.swagger.io/?url=https://raw.githubusercontent.com/sslivins/thermux/main/docs/openapi.yaml)

## Home Assistant Integration

The device automatically registers sensors with Home Assistant via MQTT discovery. Each sensor appears as a temperature entity. Diagnostics include network status, uptime, seconds since the last successful bus read, consecutive failed bus cycles, and a rolling error rate across the most recent 1,000 reads. Lifetime read counters remain available as disabled-by-default engineering diagnostics.

### Firmware Update Entity

Thermux publishes a native Home Assistant **`update` entity** (device class `firmware`) via MQTT discovery. It shows the installed version, the latest available GitHub release, a link to the release notes, and an **Install** button that triggers the OTA download **with a live progress bar** — the same experience as updating any other HA device, no automation or REST command needed.

The entity is backed by these MQTT topics (base topic configurable, default `thermux`):

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `homeassistant/update/thermux_firmware/config` | retained | Discovery config |
| `thermux/update/state` | device → HA (retained) | JSON: `installed_version`, `latest_version`, `release_url`, `update_percentage`, `in_progress` |
| `thermux/update/install` | HA → device | Payload `install` starts the OTA |

The device checks GitHub for new releases periodically (see `OTA_CHECK_INTERVAL_HOURS`) and updates the entity whenever availability or download progress changes.

### Manual REST Integration (Optional)

You can also poll sensors directly:

```yaml
# configuration.yaml
rest:
  - resource: http://thermux.local/api/sensors
    scan_interval: 60
    sensor:
      - name: "Temperature Sensor 1"
        value_template: "{{ value_json[0].temperature }}"
        unit_of_measurement: "°C"
        device_class: temperature
```

## Modbus TCP

Thermux can run a small, **read-only** Modbus TCP server so a PLC or heat pump controller can read temperatures without HTTP or JSON. It's off by default: turn it on under **Settings → Modbus TCP**, where you can also set the port (default 502) and unit ID (default 1).

Modbus has no password. Anyone on the network can read the registers, but nothing can be changed over Modbus, so only enable it on a network you trust.

### Sensor channels

Each sensor gets a **channel** (0–99) the first time it's seen, keyed by its ROM ID. A sensor keeps its channel when other sensors are added or removed, so a register address always refers to the same physical sensor. New sensors fill the lowest free channels, in ROM ID order.

The channel table on the settings page shows each channel's register, sensor, and the status and temperature Modbus clients are currently reading. From there you can:

- **Move** a sensor to another channel. If that channel is in use, the two sensors swap.
- **Release** the channel of a sensor that has been disconnected, so the channel can be reused. Channels of connected sensors can't be released; they would just be reassigned on the next read.

Channels and Modbus settings are included in backups (**Settings → Backup**), so a replacement Thermux can serve the same register map.

### Register map

All registers are **input registers**, read with function code 04. Addresses are zero-based. All 100 channels are always mapped, whatever `CONFIG_MAX_SENSORS` is set to, so the map never changes between builds.

| Address | Contents |
|---------|----------|
| 0 | Map version (currently 1) |
| 1–3 | Firmware version: major, minor, patch |
| 4 | Channel capacity (always 100) |
| 5 | Read-cycle counter. Goes up by one after every scheduled read attempt, including failed ones; wraps at 65535 |
| 6–7 | Uptime in seconds (32-bit, register 6 is the high word) |
| 8–10 | Ethernet MAC address, two bytes per register (register 8 = byte 0 << 8 \| byte 1) |
| 11 | Firmware sensor limit (`CONFIG_MAX_SENSORS`) |
| 12 | Number of channels in use |
| 13 | Number of sensors found on the bus |
| 14 | Result of the last read cycle: 0 OK, 1 some sensors failed, 2 bus failure, 3 no sensors |
| 15 | Read interval in seconds |
| 100 + channel | Temperature in hundredths of a °C, signed 16-bit (2150 = 21.50 °C, 0xFF38 = −2.00 °C). **0x8000 whenever the status isn't OK** |
| 200 + channel | Status (see below) |
| 300 + channel | Seconds since the last successful read (capped at 65534). 65535 if the sensor has never been read or isn't connected |
| 1000 + 4 × channel | ROM ID, 4 registers per channel, two bytes per register in the same order as the address shown in the UI and API |

Channel status, checked in this order:

| Value | Meaning |
|-------|---------|
| 1 | Unassigned: no sensor in this channel |
| 2 | Missing: the sensor wasn't found in the latest bus scan |
| 3 | Read error: the sensor's latest read failed, or the whole read cycle failed |
| 4 | Stale: never read successfully, or the last good reading is older than 3 × the read interval (at least 30 s) |
| 0 | OK |

Use the age registers if you need a stricter freshness limit than the stale rule.

Each block (0–15, 100–199, 200–299, 300–399, 1000–1399) is separate. A read that covers an address outside those blocks, or runs from one block into the next, returns exception 0x02 (illegal data address). A single read can cover at most 125 registers, so the ROM block takes four reads.

### Reading a consistent set

A reading cycle can finish between two of your requests. To be sure the values you read all come from the same cycle, read register 5, read the blocks you need, then read register 5 again. If it changed, read again.

### Unit ID and errors

- Address requests to the configured unit ID (default 1). Unit ID 0 is also answered.
- Requests for any other unit ID get exception 0x0B (gateway target device failed to respond).
- Unit ID 255 isn't answered: the Modbus library Thermux uses (esp-modbus 2.1.3) drops it before it reaches the server. A fix has been submitted upstream ([espressif/esp-modbus#190](https://github.com/espressif/esp-modbus/pull/190)).
- Every function code other than 04, including all writes, gets exception 0x01 (illegal function).
- Up to 3 clients can be connected at once.

### Discovery

While the server is running, Thermux advertises it over mDNS as `_mbap._tcp`, with TXT records `unit` (unit ID), `id` (MAC address) and `map` (map version). Most PLCs don't use mDNS, so you'll usually enter the IP address and port by hand; give Thermux a DHCP reservation so the address doesn't change.

### Examples

The repo includes a small reader, `scripts/thermux_modbus.py` (needs `pip install pymodbus`). With just an address it decodes the whole map; `raw` reads any range from any Modbus TCP device:

```bash
python scripts/thermux_modbus.py thermux.local               # device info + every channel
python scripts/thermux_modbus.py thermux.local --watch 5     # repeat every 5 s
python scripts/thermux_modbus.py 192.168.1.50 --port 502 --unit 1 raw 100 6          # input registers
python scripts/thermux_modbus.py 192.168.1.50 raw 0 10 --fc 3                        # holding registers
```

[mbpoll](https://github.com/epsilonrt/mbpoll), temperatures of channels 0–5 (`-0` makes addresses zero-based):

```bash
mbpoll -m tcp -a 1 -0 -t 3 -r 100 -c 6 -1 thermux.local
```

Python with [pymodbus](https://github.com/pymodbus-dev/pymodbus) 3.9 or later (older versions use `slave=` instead of `device_id=`):

```python
from pymodbus.client import ModbusTcpClient

client = ModbusTcpClient("thermux.local", port=502)
client.connect()

def read(address, count):
    result = client.read_input_registers(address, count=count, device_id=1)
    if result.isError():
        raise RuntimeError(result)
    return result.registers

while True:
    cycle = read(5, 1)[0]
    temps = read(100, 10)
    status = read(200, 10)
    if read(5, 1)[0] == cycle:
        break

for channel, (raw, st) in enumerate(zip(temps, status)):
    if st == 0:
        celsius = (raw - 65536 if raw >= 32768 else raw) / 100
        print(f"channel {channel}: {celsius:.2f} °C")
```

## OTA Updates

Thermux can update three ways: the native Home Assistant **update entity** (above), the built-in **web interface**, or a **manual `.bin` upload**.

### Automatic Updates (Recommended)

The web interface can automatically download and install updates from GitHub releases:

1. Configure the OTA URL in web settings: `https://api.github.com/repos/sslivins/thermux/releases/latest`
2. Go to the OTA page and click "Check for Updates"
3. If a newer version is available, click "Update Now"
4. The device downloads, flashes, and reboots automatically - **no manual file download needed**

### Manual Upload

To install a **specific version** or **older version**, use the manual upload feature:

1. Download the desired `thermux.bin` from any [release](https://github.com/sslivins/thermux/releases)
2. Navigate to `/ota` page
3. Select the `.bin` firmware file
4. Click "Upload & Flash"

## Security

By default, the web interface is open (no authentication required). To enable password protection:

1. Go to `/config` → Security section
2. Check "Enable password protection"
3. Set username and password
4. Click "Save Security Settings"

When enabled, unauthorized access redirects to a login page. Sessions are stored in a cookie and expire after 7 days.

### API Key Authentication

For scripts and automation, use the API key instead of session cookies:

```bash
# Get sensor readings
curl -H "X-API-Key: YOUR_API_KEY" http://thermux.local/api/sensors

# Get device status
curl -H "X-API-Key: YOUR_API_KEY" http://thermux.local/api/status
```

The API key is:
- **Auto-generated** when authentication is first enabled
- **Visible** in Settings → Security section (click 📋 to copy)
- **Regeneratable** if compromised (click 🔄 to generate new key)
- **Persisted** across reboots in NVS flash

### Session Authentication

For browser-based access, the web UI uses session cookies. Protected API endpoints return `401 Unauthorized` with JSON body containing `{"login_required": true}` when not authenticated.

#### Auth Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/login` | GET | Login page (no auth required) |
| `/api/auth/login` | POST | Authenticate with `{"username":"...", "password":"..."}` |
| `/api/auth/logout` | POST | Destroy session |
| `/api/auth/status` | GET | Check if logged in |
| `/api/config/auth` | GET | Get auth config (includes API key) |
| `/api/config/auth/regenerate-key` | POST | Generate new API key |

### Home Assistant Integration

With API key authentication, REST integrations are straightforward:

```yaml
# configuration.yaml
rest:
  - resource: http://thermux.local/api/sensors
    headers:
      X-API-Key: "your-api-key-here"
    sensor:
      - name: "Pool Temperature"
        value_template: "{{ value_json.sensors[0].temperature }}"
        unit_of_measurement: "°C"
```

Alternatively, use the built-in MQTT integration with Home Assistant auto-discovery (no API key needed for MQTT).

## Technical Notes

### Optimized Temperature Reading

The firmware uses the 1-Wire **skip ROM** command (`0xCC`) to trigger temperature conversion on all DS18B20 sensors simultaneously, then reads each sensor individually. This reduces read time from O(n × delay) to O(delay + n × read):

| Sensors | Sequential (12-bit) | Parallel (12-bit) | Parallel (9-bit) |
|---------|---------------------|-------------------|-------------------|
| 1       | ~800ms              | ~800ms            | ~100ms            |
| 5       | ~4000ms             | ~850ms            | ~150ms            |
| 10      | ~8000ms             | ~900ms            | ~250ms            |
| 20      | ~16000ms            | ~1050ms           | ~450ms            |

The conversion delay depends on resolution: 12-bit = 750ms, 11-bit = 375ms, 10-bit = 188ms, 9-bit = 94ms. The parallel read overhead per sensor is minimal (~25ms for bus communication).

### Log Buffer

A 16KB circular buffer captures ESP-IDF logs for web display. Noisy system components (HTTP server internals, Ethernet MAC, etc.) are filtered to keep logs useful. The buffer can be viewed, cleared, and downloaded from the config page.

## Hardware Design

This repository includes open-source hardware designs:

### PCB ([hardware/](hardware/))

Two open-source KiCad boards:

- **Sensor Bus Hat** - plugs onto the ESP32-POE-ISO and breaks the 1-Wire bus out to RJ45 (carries the bus pull-up resistor)
- **Temperature Node** - DS18B20 breakout with RJ45 connectors for daisy-chaining sensors

- **KiCad project files** - Full schematic and PCB layout
- **Gerber files** - Ready for fabrication (JLCPCB, PCBWay, etc.)
- **Bill of Materials** - DigiKey part links

### Enclosure ([enclosure/](enclosure/))

3D printable cases, in two parts:

- **ESP32-POE-ISO Case** - the main enclosure; houses the ESP32-POE-ISO with the Sensor Bus Hat plugged on
- **Temperature Node** - two-part case (PCB + enclosure) for a distributed DS18B20 sensor node on the 1-Wire bus

Each is provided as:

- **Fusion 360** (.f3d / .f3z) - Parametric source files
- **STEP** (.step) - Universal CAD interchange format
- **Print files** (.3mf, .stl) - Ready for slicing

## Version History

See [GitHub Releases](https://github.com/sslivins/thermux/releases) for changelog.

## License

MIT License - See [LICENSE](LICENSE) for details.
