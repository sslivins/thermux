/**
 * @file web_server.c
 * @brief HTTP web server with REST API and embedded web portal
 */

#include "web_server.h"
#include "sensor_manager.h"
#include "ota_updater.h"
#include "nvs_storage.h"
#include "onewire_temp.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

extern const char *APP_VERSION;

/* Forward declarations for reconfiguration */
extern esp_err_t mqtt_ha_stop(void);
extern esp_err_t mqtt_ha_init(void);
extern esp_err_t mqtt_ha_start(void);

/* Embedded HTML page */
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Temperature Monitor</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #fff;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 1200px; margin: 0 auto; }
        h1 { 
            text-align: center; 
            margin-bottom: 10px;
            font-size: 2em;
        }
        .version { 
            text-align: center; 
            color: #888; 
            margin-bottom: 30px;
            font-size: 0.9em;
        }
        .status-bar {
            display: flex;
            justify-content: center;
            gap: 20px;
            margin-bottom: 30px;
            flex-wrap: wrap;
        }
        .status-item {
            background: rgba(255,255,255,0.1);
            padding: 10px 20px;
            border-radius: 20px;
            font-size: 0.9em;
        }
        .status-online { color: #4ade80; }
        .status-offline { color: #f87171; }
        .sensors-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
            gap: 20px;
        }
        .sensor-card {
            background: rgba(255,255,255,0.05);
            border-radius: 15px;
            padding: 20px;
            border: 1px solid rgba(255,255,255,0.1);
            transition: transform 0.2s, box-shadow 0.2s;
        }
        .sensor-card:hover {
            transform: translateY(-5px);
            box-shadow: 0 10px 30px rgba(0,0,0,0.3);
        }
        .sensor-temp {
            font-size: 3em;
            font-weight: 300;
            color: #60a5fa;
            margin: 10px 0;
        }
        .sensor-name {
            font-size: 1.2em;
            margin-bottom: 5px;
        }
        .sensor-address {
            font-size: 0.8em;
            color: #888;
            font-family: monospace;
        }
        .sensor-name-input {
            width: 100%;
            padding: 8px 12px;
            border: 1px solid rgba(255,255,255,0.2);
            border-radius: 8px;
            background: rgba(255,255,255,0.1);
            color: #fff;
            font-size: 1em;
            margin-top: 15px;
        }
        .sensor-name-input:focus {
            outline: none;
            border-color: #60a5fa;
        }
        .btn {
            padding: 8px 16px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 0.9em;
            transition: background 0.2s;
            margin-top: 10px;
        }
        .btn-primary {
            background: #3b82f6;
            color: white;
        }
        .btn-primary:hover { background: #2563eb; }
        .btn-secondary {
            background: rgba(255,255,255,0.1);
            color: white;
        }
        .btn-secondary:hover { background: rgba(255,255,255,0.2); }
        .actions {
            text-align: center;
            margin-top: 30px;
        }
        .toast {
            position: fixed;
            bottom: 20px;
            right: 20px;
            background: #22c55e;
            color: white;
            padding: 12px 24px;
            border-radius: 8px;
            opacity: 0;
            transition: opacity 0.3s;
        }
        .toast.show { opacity: 1; }
        .toast.error { background: #ef4444; }
        .loading { opacity: 0.5; }
        @media (max-width: 600px) {
            .sensor-temp { font-size: 2.5em; }
            h1 { font-size: 1.5em; }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌡️ Temperature Monitor</h1>
        <div class="version" id="version">Version loading...</div>
        
        <div class="status-bar">
            <div class="status-item">
                <span id="sensor-count">0</span> Sensors
            </div>
            <div class="status-item">
                MQTT: <span id="mqtt-status" class="status-offline">Offline</span>
            </div>
            <div class="status-item">
                Last Update: <span id="last-update">-</span>
            </div>
        </div>

        <div class="sensors-grid" id="sensors-grid">
            <div class="sensor-card loading">Loading sensors...</div>
        </div>

        <div class="actions">
            <button class="btn btn-secondary" onclick="rescanSensors()">🔄 Rescan Sensors</button>
            <button class="btn btn-secondary" onclick="checkOTA()">📦 Check for Updates</button>
            <button class="btn btn-secondary" onclick="location.href='/config'">⚙️ Settings</button>
        </div>
    </div>

    <div class="toast" id="toast"></div>

    <script>
        let sensors = [];
        let updateInterval;
        let isEditing = false;  // Track if user is editing an input

        async function fetchSensors() {
            // Skip update if user is editing a field
            if (isEditing) return;
            
            try {
                const response = await fetch('/api/sensors');
                sensors = await response.json();
                renderSensors();
                document.getElementById('sensor-count').textContent = sensors.length;
                document.getElementById('last-update').textContent = new Date().toLocaleTimeString();
            } catch (err) {
                showToast('Failed to fetch sensors', true);
            }
        }

        async function fetchStatus() {
            try {
                const response = await fetch('/api/status');
                const status = await response.json();
                document.getElementById('version').textContent = 'Version ' + status.version;
                document.getElementById('mqtt-status').textContent = status.mqtt_connected ? 'Online' : 'Offline';
                document.getElementById('mqtt-status').className = status.mqtt_connected ? 'status-online' : 'status-offline';
            } catch (err) {
                console.error('Failed to fetch status');
            }
        }

        function renderSensors() {
            const grid = document.getElementById('sensors-grid');
            if (sensors.length === 0) {
                grid.innerHTML = '<div class="sensor-card">No sensors found. Click "Rescan Sensors" to detect connected sensors.</div>';
                return;
            }
            
            grid.innerHTML = sensors.map(sensor => `
                <div class="sensor-card" data-address="${sensor.address}">
                    <div class="sensor-name">${sensor.friendly_name || sensor.address}</div>
                    <div class="sensor-address">${sensor.address}</div>
                    <div class="sensor-temp">${sensor.valid ? sensor.temperature.toFixed(1) + '°C' : '--.-°C'}</div>
                    <input type="text" class="sensor-name-input" 
                           placeholder="Enter friendly name" 
                           value="${sensor.friendly_name || ''}"
                           onfocus="isEditing = true"
                           onblur="isEditing = false"
                           onkeypress="if(event.key==='Enter') saveName('${sensor.address}', this.value)">
                    <button class="btn btn-primary" onclick="saveName('${sensor.address}', this.previousElementSibling.value)">
                        Save Name
                    </button>
                </div>
            `).join('');
        }

        async function saveName(address, name) {
            try {
                const response = await fetch('/api/sensors/' + address + '/name', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ friendly_name: name })
                });
                if (response.ok) {
                    showToast('Name saved successfully');
                    fetchSensors();
                } else {
                    showToast('Failed to save name', true);
                }
            } catch (err) {
                showToast('Error saving name', true);
            }
        }

        async function rescanSensors() {
            try {
                showToast('Scanning for sensors...');
                const response = await fetch('/api/sensors/rescan', { method: 'POST' });
                if (response.ok) {
                    showToast('Scan complete');
                    fetchSensors();
                } else {
                    showToast('Scan failed', true);
                }
            } catch (err) {
                showToast('Error during scan', true);
            }
        }

        async function checkOTA() {
            try {
                showToast('Checking for updates...');
                const response = await fetch('/api/ota/check', { method: 'POST' });
                const result = await response.json();
                if (result.update_available) {
                    if (confirm('Update available: ' + result.latest_version + '. Install now?')) {
                        fetch('/api/ota/update', { method: 'POST' });
                        showToast('Update started. Device will restart.');
                    }
                } else {
                    showToast('Already up to date: ' + result.current_version);
                }
            } catch (err) {
                showToast('Error checking for updates', true);
            }
        }

        function showToast(message, isError = false) {
            const toast = document.getElementById('toast');
            toast.textContent = message;
            toast.className = 'toast show' + (isError ? ' error' : '');
            setTimeout(() => toast.className = 'toast', 3000);
        }

        // Initial load
        fetchStatus();
        fetchSensors();
        
        // Auto-refresh every 5 seconds
        updateInterval = setInterval(fetchSensors, 5000);
    </script>
</body>
</html>
)rawliteral";

/* Configuration page HTML */
static const char CONFIG_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Settings</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #fff;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 800px; margin: 0 auto; }
        h1 { text-align: center; margin-bottom: 10px; font-size: 2em; }
        .back-link { text-align: center; margin-bottom: 30px; }
        .back-link a { color: #60a5fa; text-decoration: none; }
        .back-link a:hover { text-decoration: underline; }
        .config-section {
            background: rgba(255,255,255,0.05);
            border-radius: 15px;
            padding: 25px;
            margin-bottom: 20px;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .section-title {
            font-size: 1.3em;
            margin-bottom: 20px;
            color: #60a5fa;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .form-group {
            margin-bottom: 15px;
        }
        .form-group label {
            display: block;
            margin-bottom: 5px;
            color: #ccc;
            font-size: 0.9em;
        }
        .form-group input {
            width: 100%;
            padding: 12px 15px;
            border: 1px solid rgba(255,255,255,0.2);
            border-radius: 8px;
            background: rgba(255,255,255,0.1);
            color: #fff;
            font-size: 1em;
        }
        .form-group input:focus {
            outline: none;
            border-color: #60a5fa;
        }
        .form-group input::placeholder { color: #666; }
        .form-hint {
            font-size: 0.8em;
            color: #888;
            margin-top: 5px;
        }
        .btn {
            padding: 12px 24px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 1em;
            transition: background 0.2s;
            margin-right: 10px;
            margin-top: 10px;
        }
        .btn-primary { background: #3b82f6; color: white; }
        .btn-primary:hover { background: #2563eb; }
        .btn-danger { background: #ef4444; color: white; }
        .btn-danger:hover { background: #dc2626; }
        .btn-secondary { background: rgba(255,255,255,0.1); color: white; }
        .btn-secondary:hover { background: rgba(255,255,255,0.2); }
        .status-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 12px;
            font-size: 0.8em;
            margin-left: 10px;
        }
        .status-connected { background: #22c55e; }
        .status-disconnected { background: #ef4444; }
        .toast {
            position: fixed;
            bottom: 20px;
            right: 20px;
            background: #22c55e;
            color: white;
            padding: 12px 24px;
            border-radius: 8px;
            opacity: 0;
            transition: opacity 0.3s;
            z-index: 1000;
        }
        .toast.show { opacity: 1; }
        .toast.error { background: #ef4444; }
        .current-value {
            font-size: 0.85em;
            color: #888;
            margin-bottom: 8px;
        }
        .danger-zone {
            border-color: #ef4444;
        }
        .danger-zone .section-title { color: #ef4444; }
    </style>
</head>
<body>
    <div class="container">
        <h1>⚙️ Settings</h1>
        <div class="back-link"><a href="/">← Back to Dashboard</a></div>

        <!-- WiFi Configuration -->
        <div class="config-section">
            <div class="section-title">📶 WiFi Configuration</div>
            <form id="wifi-form">
                <div class="form-group">
                    <label for="wifi-ssid">Network Name (SSID)</label>
                    <div class="current-value" id="wifi-ssid-current">Current: Loading...</div>
                    <input type="text" id="wifi-ssid" name="ssid" placeholder="Enter WiFi network name" maxlength="31">
                </div>
                <div class="form-group">
                    <label for="wifi-password">Password</label>
                    <input type="password" id="wifi-password" name="password" placeholder="Enter WiFi password" maxlength="63">
                    <div class="form-hint">Leave blank to keep current password</div>
                </div>
                <button type="submit" class="btn btn-primary">💾 Save WiFi Settings</button>
            </form>
        </div>

        <!-- MQTT Configuration -->
        <div class="config-section">
            <div class="section-title">
                🔗 MQTT Configuration
                <span id="mqtt-status" class="status-badge status-disconnected">Disconnected</span>
            </div>
            <form id="mqtt-form">
                <div class="form-group">
                    <label for="mqtt-uri">Broker URI</label>
                    <div class="current-value" id="mqtt-uri-current">Current: Loading...</div>
                    <input type="text" id="mqtt-uri" name="uri" placeholder="mqtt://192.168.1.100:1883">
                    <div class="form-hint">Example: mqtt://host:1883 or mqtts://host:8883 for TLS</div>
                </div>
                <div class="form-group">
                    <label for="mqtt-username">Username (optional)</label>
                    <div class="current-value" id="mqtt-user-current">Current: Loading...</div>
                    <input type="text" id="mqtt-username" name="username" placeholder="MQTT username">
                </div>
                <div class="form-group">
                    <label for="mqtt-password">Password (optional)</label>
                    <input type="password" id="mqtt-password" name="password" placeholder="MQTT password">
                    <div class="form-hint">Leave blank to keep current password</div>
                </div>
                <button type="submit" class="btn btn-primary">💾 Save MQTT Settings</button>
                <button type="button" class="btn btn-secondary" onclick="reconnectMqtt()">🔄 Reconnect</button>
            </form>
        </div>

        <!-- Sensor Configuration -->
        <div class="config-section">
            <div class="section-title">🌡️ Sensor Configuration</div>
            <form id="sensor-form">
                <div class="form-group">
                    <label for="read-interval">Read Interval (seconds)</label>
                    <div class="current-value" id="read-interval-current">Current: Loading...</div>
                    <input type="number" id="read-interval" name="read_interval" min="1" max="300" placeholder="10">
                    <div class="form-hint">How often to read temperature from sensors (1-300 seconds)</div>
                </div>
                <div class="form-group">
                    <label for="publish-interval">MQTT Publish Interval (seconds)</label>
                    <div class="current-value" id="publish-interval-current">Current: Loading...</div>
                    <input type="number" id="publish-interval" name="publish_interval" min="5" max="600" placeholder="10">
                    <div class="form-hint">How often to send readings to Home Assistant (5-600 seconds)</div>
                </div>
                <div class="form-group">
                    <label for="resolution">Sensor Resolution</label>
                    <div class="current-value" id="resolution-current">Current: Loading...</div>
                    <select id="resolution" name="resolution">
                        <option value="9">9-bit (0.5°C, ~94ms)</option>
                        <option value="10">10-bit (0.25°C, ~188ms)</option>
                        <option value="11">11-bit (0.125°C, ~375ms)</option>
                        <option value="12">12-bit (0.0625°C, ~750ms)</option>
                    </select>
                    <div class="form-hint">Higher resolution = more precision but slower readings</div>
                </div>
                <button type="submit" class="btn btn-primary">💾 Save Sensor Settings</button>
            </form>
        </div>

        <!-- System Actions -->
        <div class="config-section danger-zone">
            <div class="section-title">⚠️ System</div>
            <p style="margin-bottom: 15px; color: #ccc;">Device restart is required after changing WiFi settings.</p>
            <button class="btn btn-secondary" onclick="restartDevice()">🔄 Restart Device</button>
            <button class="btn btn-danger" onclick="factoryReset()">🗑️ Factory Reset</button>
        </div>
    </div>

    <div class="toast" id="toast"></div>

    <script>
        async function loadConfig() {
            try {
                // Load WiFi config
                const wifiResp = await fetch('/api/config/wifi');
                const wifi = await wifiResp.json();
                document.getElementById('wifi-ssid-current').textContent = 'Current: ' + (wifi.ssid || 'Not set');
                document.getElementById('wifi-ssid').placeholder = wifi.ssid || 'Enter WiFi network name';

                // Load MQTT config
                const mqttResp = await fetch('/api/config/mqtt');
                const mqtt = await mqttResp.json();
                document.getElementById('mqtt-uri-current').textContent = 'Current: ' + (mqtt.uri || 'Not set');
                document.getElementById('mqtt-user-current').textContent = 'Current: ' + (mqtt.username || 'None');
                document.getElementById('mqtt-uri').placeholder = mqtt.uri || 'mqtt://host:1883';
                document.getElementById('mqtt-username').placeholder = mqtt.username || 'Username';
                
                // Update MQTT status
                const statusResp = await fetch('/api/status');
                const status = await statusResp.json();
                const mqttBadge = document.getElementById('mqtt-status');
                mqttBadge.textContent = status.mqtt_connected ? 'Connected' : 'Disconnected';
                mqttBadge.className = 'status-badge ' + (status.mqtt_connected ? 'status-connected' : 'status-disconnected');

                // Load Sensor config
                const sensorResp = await fetch('/api/config/sensor');
                const sensor = await sensorResp.json();
                document.getElementById('read-interval-current').textContent = 'Current: ' + (sensor.read_interval / 1000) + ' seconds';
                document.getElementById('publish-interval-current').textContent = 'Current: ' + (sensor.publish_interval / 1000) + ' seconds';
                document.getElementById('resolution-current').textContent = 'Current: ' + sensor.resolution + '-bit';
                document.getElementById('read-interval').value = sensor.read_interval / 1000;
                document.getElementById('publish-interval').value = sensor.publish_interval / 1000;
                document.getElementById('resolution').value = sensor.resolution;
            } catch (err) {
                showToast('Failed to load configuration', true);
            }
        }

        document.getElementById('wifi-form').addEventListener('submit', async (e) => {
            e.preventDefault();
            const ssid = document.getElementById('wifi-ssid').value;
            const password = document.getElementById('wifi-password').value;
            
            if (!ssid) {
                showToast('Please enter WiFi SSID', true);
                return;
            }
            
            try {
                const resp = await fetch('/api/config/wifi', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password })
                });
                if (resp.ok) {
                    showToast('WiFi settings saved. Restart to apply.');
                    loadConfig();
                } else {
                    showToast('Failed to save WiFi settings', true);
                }
            } catch (err) {
                showToast('Error saving WiFi settings', true);
            }
        });

        document.getElementById('mqtt-form').addEventListener('submit', async (e) => {
            e.preventDefault();
            const uri = document.getElementById('mqtt-uri').value;
            const username = document.getElementById('mqtt-username').value;
            const password = document.getElementById('mqtt-password').value;
            
            if (!uri) {
                showToast('Please enter MQTT broker URI', true);
                return;
            }
            
            try {
                const resp = await fetch('/api/config/mqtt', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ uri, username, password })
                });
                if (resp.ok) {
                    showToast('MQTT settings saved');
                    loadConfig();
                } else {
                    showToast('Failed to save MQTT settings', true);
                }
            } catch (err) {
                showToast('Error saving MQTT settings', true);
            }
        });

        async function reconnectMqtt() {
            try {
                showToast('Reconnecting MQTT...');
                const resp = await fetch('/api/mqtt/reconnect', { method: 'POST' });
                if (resp.ok) {
                    showToast('MQTT reconnecting...');
                    setTimeout(loadConfig, 3000);
                } else {
                    showToast('Failed to reconnect', true);
                }
            } catch (err) {
                showToast('Error reconnecting MQTT', true);
            }
        }

        async function restartDevice() {
            if (!confirm('Are you sure you want to restart the device?')) return;
            try {
                await fetch('/api/system/restart', { method: 'POST' });
                showToast('Device restarting...');
            } catch (err) {
                showToast('Restart command sent');
            }
        }

        async function factoryReset() {
            if (!confirm('⚠️ This will erase ALL settings including sensor names!\n\nAre you sure?')) return;
            if (!confirm('Last chance! This cannot be undone. Continue?')) return;
            try {
                const resp = await fetch('/api/system/factory-reset', { method: 'POST' });
                if (resp.ok) {
                    showToast('Factory reset complete. Restarting...');
                } else {
                    showToast('Factory reset failed', true);
                }
            } catch (err) {
                showToast('Factory reset initiated');
            }
        }

        document.getElementById('sensor-form').addEventListener('submit', async (e) => {
            e.preventDefault();
            const readInterval = parseInt(document.getElementById('read-interval').value) * 1000;
            const publishInterval = parseInt(document.getElementById('publish-interval').value) * 1000;
            const resolution = parseInt(document.getElementById('resolution').value);
            
            if (readInterval < 1000 || readInterval > 300000) {
                showToast('Read interval must be 1-300 seconds', true);
                return;
            }
            if (publishInterval < 5000 || publishInterval > 600000) {
                showToast('Publish interval must be 5-600 seconds', true);
                return;
            }
            
            try {
                const resp = await fetch('/api/config/sensor', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ 
                        read_interval: readInterval, 
                        publish_interval: publishInterval,
                        resolution: resolution
                    })
                });
                if (resp.ok) {
                    showToast('Sensor settings saved');
                    loadConfig();
                } else {
                    showToast('Failed to save sensor settings', true);
                }
            } catch (err) {
                showToast('Error saving sensor settings', true);
            }
        });

        function showToast(message, isError = false) {
            const toast = document.getElementById('toast');
            toast.textContent = message;
            toast.className = 'toast show' + (isError ? ' error' : '');
            setTimeout(() => toast.className = 'toast', 3000);
        }

        // Load config on page load
        loadConfig();
    </script>
</body>
</html>
)rawliteral";

/**
 * @brief Handler for GET /
 */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/status
 */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    cJSON_AddNumberToObject(root, "sensor_count", sensor_manager_get_count());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_log_timestamp() / 1000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    
    extern bool mqtt_ha_is_connected(void);
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_ha_is_connected());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/sensors
 */
static esp_err_t api_sensors_get_handler(httpd_req_t *req)
{
    int count;
    const managed_sensor_t *sensors = sensor_manager_get_sensors(&count);

    cJSON *root = cJSON_CreateArray();
    
    for (int i = 0; i < count; i++) {
        cJSON *sensor = cJSON_CreateObject();
        cJSON_AddStringToObject(sensor, "address", sensors[i].address_str);
        cJSON_AddNumberToObject(sensor, "temperature", sensors[i].hw_sensor.temperature);
        cJSON_AddBoolToObject(sensor, "valid", sensors[i].hw_sensor.valid);
        
        if (sensors[i].has_friendly_name) {
            cJSON_AddStringToObject(sensor, "friendly_name", sensors[i].friendly_name);
        } else {
            cJSON_AddNullToObject(sensor, "friendly_name");
        }
        
        cJSON_AddItemToArray(root, sensor);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/rescan
 */
static esp_err_t api_sensors_rescan_handler(httpd_req_t *req)
{
    esp_err_t err = sensor_manager_rescan();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddNumberToObject(root, "sensor_count", sensor_manager_get_count());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/sensors/:address/name
 */
static esp_err_t api_sensor_name_handler(httpd_req_t *req)
{
    /* Extract address from URI */
    char address[20] = {0};
    const char *uri = req->uri;
    
    /* URI format: /api/sensors/XXXX/name */
    const char *start = strstr(uri, "/api/sensors/");
    if (start) {
        start += strlen("/api/sensors/");
        const char *end = strstr(start, "/name");
        if (end && (end - start) < sizeof(address)) {
            strncpy(address, start, end - start);
        }
    }

    ESP_LOGI("web_server", "Set name request for address: '%s'", address);

    if (strlen(address) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid address");
        return ESP_FAIL;
    }

    /* Read request body */
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI("web_server", "Request body: %s", content);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name_item = cJSON_GetObjectItem(root, "friendly_name");
    if (!cJSON_IsString(name_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing friendly_name");
        return ESP_FAIL;
    }

    esp_err_t err = sensor_manager_set_friendly_name(address, name_item->valuestring);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE("web_server", "Failed to set friendly name: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sensor not found");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/check
 */
static esp_err_t api_ota_check_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    bool update_available = false;
    char latest_version[32] = {0};
    
    esp_err_t err = ota_check_for_update();
    update_available = ota_is_update_available();
    ota_get_latest_version(latest_version, sizeof(latest_version));
    
    cJSON_AddBoolToObject(root, "update_available", update_available);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddStringToObject(root, "latest_version", latest_version);
#else
    cJSON_AddBoolToObject(root, "update_available", false);
    cJSON_AddStringToObject(root, "current_version", APP_VERSION);
    cJSON_AddStringToObject(root, "error", "OTA disabled");
#endif

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/ota/update
 */
static esp_err_t api_ota_update_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
#if CONFIG_OTA_ENABLED
    if (ota_is_update_available()) {
        cJSON_AddBoolToObject(root, "started", true);
        cJSON_AddStringToObject(root, "message", "Update starting, device will restart");
        
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
        free(json);
        
        /* Start OTA in background */
        ota_start_update();
    } else {
        cJSON_AddBoolToObject(root, "started", false);
        cJSON_AddStringToObject(root, "message", "No update available");
        
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
        free(json);
    }
#else
    cJSON_AddBoolToObject(root, "started", false);
    cJSON_AddStringToObject(root, "error", "OTA disabled");
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
#endif
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /config
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, strlen(CONFIG_HTML));
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/wifi
 */
static esp_err_t api_config_wifi_get_handler(httpd_req_t *req)
{
    char ssid[32] = {0};
    char password[64] = {0};
    
    /* Try NVS first, then menuconfig defaults */
    esp_err_t err = nvs_storage_load_wifi_config(ssid, sizeof(ssid), 
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(ssid) == 0) {
#ifdef CONFIG_WIFI_SSID
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
#endif
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    /* Don't send password for security */
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/wifi
 */
static esp_err_t api_config_wifi_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(root, "password");
    
    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    
    /* If password not provided, load existing one */
    char password[64] = {0};
    if (cJSON_IsString(password_item) && strlen(password_item->valuestring) > 0) {
        strncpy(password, password_item->valuestring, sizeof(password) - 1);
    } else {
        char existing_ssid[32];
        nvs_storage_load_wifi_config(existing_ssid, sizeof(existing_ssid),
                                      password, sizeof(password));
    }
    
    esp_err_t err = nvs_storage_save_wifi_config(ssid_item->valuestring, password);
    cJSON_Delete(root);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "WiFi config saved. Restart to apply.");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for GET /api/config/mqtt
 */
static esp_err_t api_config_mqtt_get_handler(httpd_req_t *req)
{
    char uri[128] = {0};
    char username[64] = {0};
    char password[64] = {0};
    
    esp_err_t err = nvs_storage_load_mqtt_config(uri, sizeof(uri),
                                                  username, sizeof(username),
                                                  password, sizeof(password));
    if (err != ESP_OK || strlen(uri) == 0) {
#ifdef CONFIG_MQTT_BROKER_URI
        strncpy(uri, CONFIG_MQTT_BROKER_URI, sizeof(uri) - 1);
#endif
#ifdef CONFIG_MQTT_USERNAME
        strncpy(username, CONFIG_MQTT_USERNAME, sizeof(username) - 1);
#endif
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uri", uri);
    cJSON_AddStringToObject(root, "username", username);
    /* Don't send password for security */
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/mqtt
 */
static esp_err_t api_config_mqtt_post_handler(httpd_req_t *req)
{
    char content[384];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *uri_item = cJSON_GetObjectItem(root, "uri");
    cJSON *username_item = cJSON_GetObjectItem(root, "username");
    cJSON *password_item = cJSON_GetObjectItem(root, "password");
    
    if (!cJSON_IsString(uri_item) || strlen(uri_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing uri");
        return ESP_FAIL;
    }
    
    const char *username = "";
    const char *password = "";
    char existing_password[64] = {0};
    
    if (cJSON_IsString(username_item)) {
        username = username_item->valuestring;
    }
    
    if (cJSON_IsString(password_item) && strlen(password_item->valuestring) > 0) {
        password = password_item->valuestring;
    } else {
        /* Load existing password */
        char existing_uri[128], existing_user[64];
        nvs_storage_load_mqtt_config(existing_uri, sizeof(existing_uri),
                                      existing_user, sizeof(existing_user),
                                      existing_password, sizeof(existing_password));
        password = existing_password;
    }
    
    esp_err_t err = nvs_storage_save_mqtt_config(uri_item->valuestring, username, password);
    cJSON_Delete(root);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "MQTT config saved");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/mqtt/reconnect
 */
static esp_err_t api_mqtt_reconnect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "MQTT reconnect requested");
    
    /* Stop and reinitialize MQTT with new settings */
    mqtt_ha_stop();
    mqtt_ha_init();
    mqtt_ha_start();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "MQTT reconnecting");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/* External accessor functions from main.c */
extern uint32_t get_sensor_read_interval(void);
extern uint32_t get_sensor_publish_interval(void);
extern void set_sensor_read_interval(uint32_t ms);
extern void set_sensor_publish_interval(uint32_t ms);

/**
 * @brief Handler for GET /api/config/sensor
 */
static esp_err_t api_config_sensor_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "read_interval", get_sensor_read_interval());
    cJSON_AddNumberToObject(root, "publish_interval", get_sensor_publish_interval());
    cJSON_AddNumberToObject(root, "resolution", onewire_temp_get_resolution());
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/config/sensor
 */
static esp_err_t api_config_sensor_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *read_item = cJSON_GetObjectItem(root, "read_interval");
    cJSON *publish_item = cJSON_GetObjectItem(root, "publish_interval");
    cJSON *resolution_item = cJSON_GetObjectItem(root, "resolution");
    
    uint32_t read_interval = get_sensor_read_interval();
    uint32_t publish_interval = get_sensor_publish_interval();
    uint8_t resolution = onewire_temp_get_resolution();
    
    if (cJSON_IsNumber(read_item)) {
        read_interval = (uint32_t)read_item->valueint;
        if (read_interval < 1000) read_interval = 1000;
        if (read_interval > 300000) read_interval = 300000;
        set_sensor_read_interval(read_interval);
    }
    
    if (cJSON_IsNumber(publish_item)) {
        publish_interval = (uint32_t)publish_item->valueint;
        if (publish_interval < 5000) publish_interval = 5000;
        if (publish_interval > 600000) publish_interval = 600000;
        set_sensor_publish_interval(publish_interval);
    }
    
    if (cJSON_IsNumber(resolution_item)) {
        resolution = (uint8_t)resolution_item->valueint;
        if (resolution >= 9 && resolution <= 12) {
            onewire_temp_set_resolution(resolution);
        }
    }
    
    cJSON_Delete(root);
    
    /* Save to NVS */
    esp_err_t err = nvs_storage_save_sensor_settings(read_interval, publish_interval, resolution);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Sensor settings saved");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/system/restart
 */
static esp_err_t api_system_restart_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "System restart requested");
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Restarting...");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    /* Delay restart to allow response to be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}

/**
 * @brief Handler for POST /api/system/factory-reset
 */
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested");
    
    esp_err_t err = nvs_storage_factory_reset();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(response, "message", "Factory reset complete. Restarting...");
    } else {
        cJSON_AddStringToObject(response, "error", "Factory reset failed");
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    
    if (err == ESP_OK) {
        /* Delay restart to allow response to be sent */
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "Starting web server on port %d", CONFIG_WEB_SERVER_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;  /* Increased for config endpoints */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return err;
    }

    /* Register URI handlers */
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t sensors_uri = {
        .uri = "/api/sensors",
        .method = HTTP_GET,
        .handler = api_sensors_get_handler,
    };
    httpd_register_uri_handler(s_server, &sensors_uri);

    httpd_uri_t rescan_uri = {
        .uri = "/api/sensors/rescan",
        .method = HTTP_POST,
        .handler = api_sensors_rescan_handler,
    };
    httpd_register_uri_handler(s_server, &rescan_uri);

    httpd_uri_t sensor_name_uri = {
        .uri = "/api/sensors/*",
        .method = HTTP_POST,
        .handler = api_sensor_name_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_name_uri);

    httpd_uri_t ota_check_uri = {
        .uri = "/api/ota/check",
        .method = HTTP_POST,
        .handler = api_ota_check_handler,
    };
    httpd_register_uri_handler(s_server, &ota_check_uri);

    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota/update",
        .method = HTTP_POST,
        .handler = api_ota_update_handler,
    };
    httpd_register_uri_handler(s_server, &ota_update_uri);

    /* Configuration page */
    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(s_server, &config_uri);

    /* WiFi config endpoints */
    httpd_uri_t wifi_config_get_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_GET,
        .handler = api_config_wifi_get_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_config_get_uri);

    httpd_uri_t wifi_config_post_uri = {
        .uri = "/api/config/wifi",
        .method = HTTP_POST,
        .handler = api_config_wifi_post_handler,
    };
    httpd_register_uri_handler(s_server, &wifi_config_post_uri);

    /* MQTT config endpoints */
    httpd_uri_t mqtt_config_get_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_GET,
        .handler = api_config_mqtt_get_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_config_get_uri);

    httpd_uri_t mqtt_config_post_uri = {
        .uri = "/api/config/mqtt",
        .method = HTTP_POST,
        .handler = api_config_mqtt_post_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_config_post_uri);

    httpd_uri_t mqtt_reconnect_uri = {
        .uri = "/api/mqtt/reconnect",
        .method = HTTP_POST,
        .handler = api_mqtt_reconnect_handler,
    };
    httpd_register_uri_handler(s_server, &mqtt_reconnect_uri);

    /* Sensor config endpoints */
    httpd_uri_t sensor_config_get_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_GET,
        .handler = api_config_sensor_get_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_config_get_uri);

    httpd_uri_t sensor_config_post_uri = {
        .uri = "/api/config/sensor",
        .method = HTTP_POST,
        .handler = api_config_sensor_post_handler,
    };
    httpd_register_uri_handler(s_server, &sensor_config_post_uri);

    /* System endpoints */
    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = api_system_restart_handler,
    };
    httpd_register_uri_handler(s_server, &system_restart_uri);

    httpd_uri_t factory_reset_uri = {
        .uri = "/api/system/factory-reset",
        .method = HTTP_POST,
        .handler = api_system_factory_reset_handler,
    };
    httpd_register_uri_handler(s_server, &factory_reset_uri);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
