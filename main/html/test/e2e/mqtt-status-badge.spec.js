// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the MQTT connection-status badge on both the home
 * page (index.html) and the Settings page (config.html). The badge now has
 * three states driven by /api/status:
 *   - connected      -> "Online"/"Connected"
 *   - connecting     -> amber "Reconnecting…" (esp-mqtt auto-retries forever)
 *   - disconnected   -> "Offline"/"Disconnected"
 * and surfaces the last connection error (as a tooltip on the home page and
 * as a hint line on the Settings page).
 */

test.describe('MQTT status badge', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('home page shows amber Reconnecting with error tooltip', async ({ page }) => {
        mock = await startMockServer();
        mock.state.status.mqtt_connected = false;
        mock.state.status.mqtt_connecting = true;
        mock.state.status.mqtt_last_error = 'DNS: cannot resolve broker host';
        mock.state.status.mqtt_last_error_age_sec = 12;

        await page.goto(`${mock.baseURL}/`);

        const badge = page.locator('#mqtt-status');
        await expect(badge).toHaveText('Reconnecting…');
        await expect(badge).toHaveClass(/status-connecting/);
        await expect(badge).toHaveAttribute('title', 'DNS: cannot resolve broker host');
    });

    test('home page shows Online when connected', async ({ page }) => {
        mock = await startMockServer();
        mock.state.status.mqtt_connected = true;
        mock.state.status.mqtt_connecting = false;

        await page.goto(`${mock.baseURL}/`);

        const badge = page.locator('#mqtt-status');
        await expect(badge).toHaveText('Online');
        await expect(badge).toHaveClass(/status-online/);
    });

    test('home page shows Offline when not connecting', async ({ page }) => {
        mock = await startMockServer();
        mock.state.status.mqtt_connected = false;
        mock.state.status.mqtt_connecting = false;

        await page.goto(`${mock.baseURL}/`);

        const badge = page.locator('#mqtt-status');
        await expect(badge).toHaveText('Offline');
        await expect(badge).toHaveClass(/status-offline/);
    });

    test('settings page shows Reconnecting badge and error hint line', async ({ page }) => {
        mock = await startMockServer();
        mock.state.status.mqtt_connected = false;
        mock.state.status.mqtt_connecting = true;
        mock.state.status.mqtt_last_error = 'Broker refused: not authorized';
        mock.state.status.mqtt_last_error_age_sec = 3;

        await page.goto(`${mock.baseURL}/config`);

        const badge = page.locator('#mqtt-status');
        await expect(badge).toHaveText('Reconnecting…');
        await expect(badge).toHaveClass(/status-connecting/);

        const hint = page.locator('#mqtt-error-hint');
        await expect(hint).toBeVisible();
        await expect(hint).toContainText('Broker refused: not authorized');
    });

    test('settings page hides error hint when connected', async ({ page }) => {
        mock = await startMockServer();
        mock.state.status.mqtt_connected = true;
        mock.state.status.mqtt_connecting = false;
        mock.state.status.mqtt_last_error = '';

        await page.goto(`${mock.baseURL}/config`);

        const badge = page.locator('#mqtt-status');
        await expect(badge).toHaveText('Connected');
        await expect(badge).toHaveClass(/status-connected/);
        await expect(page.locator('#mqtt-error-hint')).toBeHidden();
    });
});
