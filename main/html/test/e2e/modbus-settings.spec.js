// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the Modbus TCP settings card: that it shows the
 * device's settings and state, sends the right body on save, and reports a
 * failed restart (e.g. a port that won't bind) instead of claiming success.
 */

test.describe('Modbus TCP settings', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('shows the current settings and a running badge', async ({ page }) => {
        mock = await startMockServer();
        Object.assign(mock.state.modbus, {
            enabled: true, running: true, port: 1502, unit_id: 7,
            requests: 40, last_request_age_s: 3,
        });
        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#modbus-status')).toHaveText('Running');
        await expect(page.locator('#modbus-enabled')).toBeChecked();
        await expect(page.locator('#modbus-port')).toHaveValue('1502');
        await expect(page.locator('#modbus-unit-id')).toHaveValue('7');
        await expect(page.locator('#modbus-info')).toContainText('Requests served: 40');
    });

    test('saving sends enabled, port and unit ID', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#modbus-info')).not.toHaveText('Loading...');

        await page.locator('#modbus-enabled').check();
        await page.locator('#modbus-port').fill('5020');
        await page.locator('#modbus-unit-id').fill('3');
        await page.locator('#modbus-form button[type="submit"]').click();

        await expect(page.locator('#toast')).toContainText('Modbus settings saved');
        await expect(page.locator('#modbus-status')).toHaveText('Running');
        const post = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/modbus');
        expect(post, 'expected a POST /api/modbus request').toBeTruthy();
        expect(JSON.parse(post.body)).toEqual({ enabled: true, port: 5020, unit_id: 3 });
    });

    test('an out-of-range unit ID is blocked without calling the device', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#modbus-info')).not.toHaveText('Loading...');

        await page.locator('#modbus-unit-id').fill('248');
        await page.locator('#modbus-form button[type="submit"]').click();

        /* The input's min/max stop the submit before any request is made */
        expect(await page.locator('#modbus-unit-id').evaluate((el) => el.validity.valid)).toBe(false);
        await page.waitForTimeout(200);
        expect(mock.requests.some((r) => r.method === 'POST' && r.path === '/api/modbus')).toBe(false);
    });

    test('a failed restart shows the device error', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusFailMessage = 'Could not apply Modbus settings (ESP_FAIL); previous settings kept';
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#modbus-info')).not.toHaveText('Loading...');

        await page.locator('#modbus-enabled').check();
        await page.locator('#modbus-form button[type="submit"]').click();

        await expect(page.locator('#toast')).toContainText('previous settings kept');
    });
});
