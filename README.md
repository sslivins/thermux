# ESP32-POE Temperature Monitor

A multi-sensor temperature monitoring system for ESP32-POE boards with Home Assistant integration via MQTT auto-discovery. **Supports up to 20 DS18B20 sensors on a single 1-Wire bus** - perfect for monitoring multiple zones, equipment, or environments from one device.

## Features

- **Up to 20 DS18B20 Sensors** - Monitor multiple temperature points from a single device on one 1-Wire bus
- **Optimized Parallel Reads** - Uses 1-Wire skip ROM command to read all sensors simultaneously (~240ms for 10 sensors vs ~1200ms sequential)
- **Home Assistant Integration** - MQTT auto-discovery for seamless integration
- **Web Interface** - Configuration and monitoring via built-in web server
- **Sensor Identification** - Change detection highlighting helps identify which physical sensor is which
- **Custom Sensor Names** - Assign friendly names to sensors via web UI (persisted in NVS)
- **OTA Updates** - Over-the-air firmware updates from GitHub releases or manual upload with progress display
- **Ethernet & WiFi** - Primary Ethernet with WiFi fallback
- **mDNS** - Access via `temp-monitor.local` (auto-increments on collision: temp-monitor-2.local, etc.)
- **Service Discovery** - Discoverable via `_tempmon._tcp` and `_http._tcp` services
- **Web-based Logs** - View system logs without serial connection (16KB circular buffer)
- **Runtime Log Level Control** - Change log verbosity via web UI without reflashing
- **Session-based Authentication** - Optional password protection with login page

## Hardware Requirements

- **Board**: [Olimex ESP32-POE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/) (or compatible ESP32-POE board)
- **Sensors**: DS18B20 1-Wire temperature sensors
- **Connection**: Sensors connected to GPIO4 (configurable in menuconfig)

### Wiring

| DS18B20 Pin | ESP32-POE |
|-------------|-----------|
| VCC (Red)   | 3.3V      |
| GND (Black) | GND       |
| DATA (Yellow) | GPIO4   |

> **Note**: A 4.7kΩ pull-up resistor is required between DATA and VCC.

## Building

### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- VS Code with ESP-IDF extension (recommended)

### Build & Flash

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

After flashing, access the web interface at `http://temp-monitor.local` or the device IP.

> **Note**: If multiple devices are on the network, subsequent devices will be `temp-monitor-2.local`, `temp-monitor-3.local`, etc.

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

### Sensors

#### Get All Sensors
```
GET /api/sensors
```

Returns all discovered temperature sensors with current readings:

```json
[
  {
    "address": "28FF1234567890AB",
    "temperature": 22.5,
    "valid": true,
    "friendly_name": "Living Room"
  },
  {
    "address": "28FF0987654321CD",
    "temperature": 18.3,
    "valid": true,
    "friendly_name": null
  }
]
```

#### Rescan for Sensors
```
POST /api/sensors/rescan
```

Triggers a new 1-Wire bus scan to discover sensors.

#### Set Sensor Name
```
POST /api/sensors/{address}/name
Content-Type: application/json

{"name": "Kitchen"}
```

### Configuration

#### Get/Set MQTT Config
```
GET  /api/config/mqtt
POST /api/config/mqtt
```

#### Get/Set WiFi Config
```
GET  /api/config/wifi
POST /api/config/wifi
```

#### Get/Set Sensor Config
```
GET  /api/config/sensor
POST /api/config/sensor
```

#### Get/Set OTA Config
```
GET  /api/config/ota
POST /api/config/ota
```

### System

#### Get Device Status
```
GET /api/status
```

Returns system information including version, uptime, memory, and network status.

#### Restart Device
```
POST /api/system/restart
```

Triggers a device reboot.

#### WiFi Network Scan
```
GET /api/wifi/scan
```

Returns available WiFi networks for configuration.

### Logs

#### Get Log Buffer
```
GET /api/logs
```

Returns the contents of the 16KB circular log buffer.

#### Clear Logs
```
POST /api/logs/clear
```

Clears the log buffer.

#### Get/Set Log Level
```
GET /api/logs/level
POST /api/logs/level
Content-Type: application/json

{"level": 3}
```

Log levels: 0=None, 1=Error, 2=Warning, 3=Info, 4=Debug, 5=Verbose

### Authentication

#### Get Auth Config
```
GET /api/config/auth
```

Returns `{"enabled": true, "username": "admin"}`

#### Set Auth Config
```
POST /api/config/auth
Content-Type: application/json

{"enabled": true, "username": "admin", "password": "secret"}
```

Leave password blank to keep current password when updating.

### OTA Updates

#### Check for Updates
```
GET /api/ota/check
```

#### Start Update from URL
```
POST /api/ota/update
```

#### Manual Firmware Upload
```
POST /api/ota/upload
Content-Type: application/octet-stream

<binary firmware data>
```

## Home Assistant Integration

The device automatically registers sensors with Home Assistant via MQTT discovery. Each sensor appears as a temperature entity.

### Manual REST Integration (Optional)

You can also poll sensors directly:

```yaml
# configuration.yaml
rest:
  - resource: http://tempmon-XXXX.local/api/sensors
    scan_interval: 60
    sensor:
      - name: "Temperature Sensor 1"
        value_template: "{{ value_json[0].temperature }}"
        unit_of_measurement: "°C"
        device_class: temperature
```

### Trigger OTA Update from Home Assistant

```yaml
# configuration.yaml
rest_command:
  update_temp_monitor:
    url: "http://tempmon-XXXX.local/api/ota/update"
    method: POST
```

## OTA Updates

### Automatic (GitHub Releases)

1. Configure the OTA URL in web settings (e.g., `https://api.github.com/repos/user/repo/releases/latest`)
2. Click "Check for Updates" on the OTA page
3. If a newer version is available, click "Update Now"

### Manual Upload

1. Navigate to `/ota` page
2. Select the `.bin` firmware file
3. Click "Upload & Flash"

## Security

By default, the web interface is open (no authentication required). To enable password protection:

1. Go to `/config` → Security section
2. Check "Enable password protection"
3. Set username and password
4. Click "Save Security Settings"

When enabled, unauthorized access redirects to a login page. Sessions are stored in a cookie and expire after 24 hours.

### API Authentication

Protected API endpoints return `401 Unauthorized` with JSON body containing `{"login_required": true}` when authentication is enabled but user is not logged in.

#### Auth Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/login` | GET | Login page (no auth required) |
| `/api/auth/login` | POST | Authenticate with `{"username":"...", "password":"..."}` |
| `/api/auth/logout` | POST | Destroy session |
| `/api/auth/status` | GET | Check if logged in |

### Home Assistant with Authentication

If auth is enabled, you need to manage cookies/sessions for REST integrations. For simpler setups, consider disabling auth and using network-level security (VLAN, firewall rules).

## Technical Notes

### Optimized Temperature Reading

The firmware uses the 1-Wire **skip ROM** command (`0xCC`) to trigger temperature conversion on all DS18B20 sensors simultaneously, then reads each sensor individually. This reduces read time from O(n × delay) to O(delay + n × read):

| Sensors | Sequential | Parallel |
|---------|------------|----------|
| 1       | ~800ms     | ~800ms   |
| 5       | ~4000ms    | ~850ms   |
| 10      | ~8000ms    | ~900ms   |

The delay depends on resolution (12-bit = 750ms max, but typically ~800ms total including bus operations).

### Log Buffer

A 16KB circular buffer captures ESP-IDF logs for web display. Noisy system components (HTTP server internals, Ethernet MAC, etc.) are filtered to keep logs useful. The buffer can be viewed, cleared, and downloaded from the config page.

## Version History

See [GitHub Releases](https://github.com/sslivins/temp-monitor/releases) for changelog.

## License

MIT License - See [LICENSE](LICENSE) for details.
