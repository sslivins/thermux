// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the read_interval safety clamp on the Sensor
 * Settings form. The firmware clamps read_interval up to a device-computed
 * "safe minimum" (based on sensor count + read resolution) when the user
 * asks for something too low to reliably complete a read cycle within.
 *
 * Scope: verifies config.html surfaces the clamp via a toast rather than
 * silently accepting the requested value. Does NOT re-verify the actual
 * safe-minimum formula (that lives in web_server.c / onewire_temp.c and
 * is covered by host C code review, not browser tests).
 */

test.describe('Sensor read interval safety clamp', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('saving a read interval above the safe minimum shows a normal success toast', async ({ page }) => {
        mock = await startMockServer();
        mock.state.sensor.min_safe_read_interval_ms = 1000;
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#read-interval-current')).not.toContainText('Loading');

        await page.locator('#read-interval').fill('10');
        await page.locator('#sensor-form button[type="submit"]').click();

        await expect(page.locator('#toast')).toContainText('Sensor settings saved');
    });

    test('saving a read interval below the safe minimum is clamped and shows an explanatory toast', async ({ page }) => {
        mock = await startMockServer();
        mock.state.sensor.min_safe_read_interval_ms = 8000;
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#read-interval-current')).not.toContainText('Loading');

        await page.locator('#read-interval').fill('5');
        await page.locator('#sensor-form button[type="submit"]').click();

        const sensorRequest = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/config/sensor');
        expect(sensorRequest, 'expected a POST /api/config/sensor request').toBeTruthy();
        expect(JSON.parse(sensorRequest.body).read_interval).toBe(5000);
        await expect(page.locator('#toast')).toContainText('raised to 8s');
    });
});
