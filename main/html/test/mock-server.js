/**
 * Minimal mock of the Thermux ESP32 HTTP server, for driving Playwright
 * against the real static UI files (index.html / config.html) without
 * needing real hardware.
 *
 * Deliberately NOT a faithful ESP32 simulator - business logic is already
 * covered by the C unit tests (test/) and pure-JS unit tests
 * (main/html/test/*.test.js). This mock's only job is to give the DOM/JS
 * glue something real to fetch() against, so Playwright can verify the UI
 * wires up correctly (right endpoints called, right params sent, right
 * DOM/toast/dialog behavior) - not that the backend logic is correct.
 *
 * Serves the real routes ("/" and "/config", not "index.html"/"config.html"
 * directly) to match main/web_server.c's actual URI registrations. Auth is
 * intentionally left disabled for this suite (see api_auth_status_handler /
 * check_session_auth in web_server.c: s_auth_enabled=false means all
 * requests pass through) - a dedicated auth-contract suite can extend this
 * mock later if session/login behavior needs its own coverage.
 *
 * Any /api/* route not explicitly implemented below responds with a loud
 * 501 (rather than a silent 200), so a config.html startup path that starts
 * calling a new endpoint fails immediately instead of masking a real gap.
 */
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { once } = require('node:events');

const HTML_DIR = path.join(__dirname, '..');

function defaultState() {
    return {
        status: {
            version: '3.0.0',
            sensor_count: 0,
            max_sensors: 20,
            uptime_seconds: 42,
            free_heap: 140000,
            mqtt_connected: false,
            mqtt_connecting: false,
            mqtt_last_error: '',
            mqtt_last_error_age_sec: null,
            ethernet_connected: true,
            wifi_connected: false,
            ethernet_ip: '192.168.1.205',
            wifi_ip: '',
            bus_stats: {
                total_reads: 0,
                failed_reads: 0,
                error_rate: 0,
                recent_error_rate: 0,
                consecutive_failed_cycles: 0,
                seconds_since_last_success: null,
            },
        },
        wifi: { ssid: '' },
        mqtt: { uri: 'mqtt://test-broker.local:1883', username: 'testuser' },
        sensor: { read_interval: 10000, publish_interval: 30000, resolution: 12, min_safe_read_interval_ms: 5000 },
        auth: { enabled: false, username: '', api_key: '' },
        authStatus: { auth_enabled: false, logged_in: true },
        logLevel: { level: 3, level_name: 'info' },
        otaChannel: { include_prerelease: false },
        backup: {
            schema_version: 1,
            device_version: '3.0.0',
            sensor_names: [],
            sensor_settings: { read_interval_ms: 10000, publish_interval_ms: 30000, resolution: 12 },
        },
    };
}

function sendJson(res, status, body) {
    const json = JSON.stringify(body);
    res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(json) });
    res.end(json);
}

function sendFile(res, filePath, extraHeaders) {
    const content = fs.readFileSync(filePath);
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': content.length, ...extraHeaders });
    res.end(content);
}

async function readBody(req) {
    const chunks = [];
    for await (const chunk of req) {
        chunks.push(chunk);
    }
    return Buffer.concat(chunks).toString('utf8');
}

/**
 * Creates a fresh mock server instance. Each test should create its own via
 * startMockServer()/stopMockServer() (see below) so state never leaks
 * between tests, even under parallel workers.
 */
function createMockServer() {
    const state = defaultState();
    /** Requests observed so far, for assertions (method, url, body). */
    const requests = [];

    const server = http.createServer(async (req, res) => {
        const url = new URL(req.url, 'http://localhost');
        const body = (req.method === 'POST') ? await readBody(req) : undefined;
        requests.push({ method: req.method, path: url.pathname, query: url.searchParams, body });

        if (req.method === 'GET' && url.pathname === '/') {
            return sendFile(res, path.join(HTML_DIR, 'index.html'));
        }
        if (req.method === 'GET' && url.pathname === '/config') {
            return sendFile(res, path.join(HTML_DIR, 'config.html'));
        }

        if (req.method === 'GET' && url.pathname === '/api/status') {
            return sendJson(res, 200, state.status);
        }
        if (req.method === 'GET' && url.pathname === '/api/config/wifi') {
            return sendJson(res, 200, state.wifi);
        }
        if (req.method === 'GET' && url.pathname === '/api/config/mqtt') {
            return sendJson(res, 200, state.mqtt);
        }
        if (req.method === 'GET' && url.pathname === '/api/config/sensor') {
            return sendJson(res, 200, state.sensor);
        }
        if (req.method === 'POST' && url.pathname === '/api/config/sensor') {
            let parsed;
            try {
                parsed = JSON.parse(body);
            } catch {
                return sendJson(res, 400, { error: 'Invalid JSON' });
            }
            let readInterval = typeof parsed.read_interval === 'number' ? parsed.read_interval : state.sensor.read_interval;
            const publishInterval = typeof parsed.publish_interval === 'number' ? parsed.publish_interval : state.sensor.publish_interval;
            const resolution = typeof parsed.resolution === 'number' ? parsed.resolution : state.sensor.resolution;
            if (readInterval < 5000) readInterval = 5000;
            if (readInterval > 300000) readInterval = 300000;
            const minSafe = state.sensor.min_safe_read_interval_ms || 5000;
            let clamped = false;
            if (readInterval < minSafe) {
                readInterval = minSafe;
                clamped = true;
            }
            state.sensor.read_interval = readInterval;
            state.sensor.publish_interval = publishInterval;
            state.sensor.resolution = resolution;
            return sendJson(res, 200, {
                success: true,
                message: 'Sensor settings saved',
                read_interval: readInterval,
                read_interval_clamped: clamped,
                min_safe_read_interval_ms: minSafe,
            });
        }
        if (req.method === 'GET' && url.pathname === '/api/config/auth') {
            return sendJson(res, 200, state.auth);
        }
        if (req.method === 'GET' && url.pathname === '/api/auth/status') {
            return sendJson(res, 200, state.authStatus);
        }
        if (req.method === 'GET' && url.pathname === '/api/logs/level') {
            return sendJson(res, 200, state.logLevel);
        }
        if (req.method === 'GET' && url.pathname === '/api/ota/channel') {
            return sendJson(res, 200, state.otaChannel);
        }
        if (req.method === 'POST' && url.pathname === '/api/ota/channel') {
            let parsed;
            try {
                parsed = JSON.parse(body);
            } catch {
                return sendJson(res, 400, { error: 'Invalid JSON' });
            }
            if (typeof parsed.include_prerelease !== 'boolean') {
                return sendJson(res, 400, { error: 'Missing include_prerelease' });
            }
            state.otaChannel.include_prerelease = parsed.include_prerelease;
            return sendJson(res, 200, { success: true });
        }

        if (req.method === 'GET' && url.pathname === '/api/backup') {
            const includeMqtt = url.searchParams.get('mqtt') === '1';
            const includeWifi = url.searchParams.get('wifi') === '1';
            const includeAuth = url.searchParams.get('auth') === '1';
            const payload = { ...state.backup };
            if (includeMqtt) payload.mqtt = { broker_uri: state.mqtt.uri, username: state.mqtt.username, password: 'testpass123' };
            if (includeWifi) payload.wifi = { ssid: 'test-ssid', password: 'wifi-pass' };
            if (includeAuth) payload.auth = { enabled: false, username: '', password: '', api_key: '' };
            const json = JSON.stringify(payload);
            res.writeHead(200, {
                'Content-Type': 'application/json',
                'Content-Disposition': 'attachment; filename="thermux-backup.json"',
                'Content-Length': Buffer.byteLength(json),
            });
            return res.end(json);
        }

        if (req.method === 'POST' && url.pathname === '/api/backup/restore') {
            let parsed;
            try {
                parsed = JSON.parse(body);
            } catch {
                return sendJson(res, 400, { error: 'Invalid JSON' });
            }
            return sendJson(res, 200, {
                success: true,
                sensor_names_restored: Array.isArray(parsed.sensor_names) ? parsed.sensor_names.length : 0,
                mqtt_restored: Boolean(parsed.mqtt),
                wifi_restored: Boolean(parsed.wifi),
                auth_restored: Boolean(parsed.auth),
                message: 'Restore complete. Restarting...',
            });
        }

        // Unimplemented endpoint - fail loudly instead of a silent 200, so a
        // startup path calling something new here surfaces immediately.
        res.writeHead(501, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: `mock-server: no handler for ${req.method} ${url.pathname}` }));
    });

    return { server, state, requests };
}

async function startMockServer() {
    const { server, state, requests } = createMockServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    return {
        baseURL: `http://127.0.0.1:${port}`,
        state,
        requests,
        close: () => {
            // Force-close any keep-alive sockets so `server.close()` doesn't
            // hang waiting for the browser to disconnect them itself.
            if (typeof server.closeAllConnections === 'function') {
                server.closeAllConnections();
            }
            return new Promise((resolve) => server.close(resolve));
        },
    };
}

module.exports = { startMockServer };
