// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the reworked MQTT / WiFi config form UX: current
 * values are shown edit-in-place inside the inputs (no separate "Current:"
 * line), and secrets are never echoed - the password field stays empty with
 * a dots placeholder signalling "a password is already set".
 */

test.describe('Config form current-value UX', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('MQTT broker URI and username are prefilled as editable values', async ({ page }) => {
        mock = await startMockServer();
        mock.state.mqtt.uri = 'mqtt://homeassistant.local:1883';
        mock.state.mqtt.username = 'thermux';

        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#mqtt-uri')).toHaveValue('mqtt://homeassistant.local:1883');
        await expect(page.locator('#mqtt-username')).toHaveValue('thermux');
    });

    test('MQTT password is never echoed - empty with a dots placeholder', async ({ page }) => {
        mock = await startMockServer();
        mock.state.mqtt.uri = 'mqtt://homeassistant.local:1883';
        mock.state.mqtt.username = 'thermux';

        await page.goto(`${mock.baseURL}/config`);

        const pw = page.locator('#mqtt-password');
        await expect(pw).toHaveValue('');
        await expect(pw).toHaveAttribute('placeholder', '••••••••');
        await expect(pw).toHaveAttribute('type', 'password');
    });

    test('no redundant "Current:" lines remain in the MQTT/WiFi sections', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#mqtt-uri-current')).toHaveCount(0);
        await expect(page.locator('#mqtt-user-current')).toHaveCount(0);
        await expect(page.locator('#wifi-ssid-current')).toHaveCount(0);
    });

    test('current WiFi SSID is shown as the selected dropdown option', async ({ page }) => {
        mock = await startMockServer();
        mock.state.wifi.ssid = 'HomeNetwork';

        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#wifi-ssid')).toHaveValue('HomeNetwork');
        await expect(page.locator('#wifi-ssid option:checked')).toContainText('HomeNetwork (current)');
    });
});
