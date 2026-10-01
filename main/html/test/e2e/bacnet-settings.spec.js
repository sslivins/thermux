// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

test.describe('BACnet/IP settings', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('shows the current settings and a running badge', async ({ page }) => {
        mock = await startMockServer();
        Object.assign(mock.state.bacnet, {
            enabled: true, running: true, udp_port: 47809, device_instance: 9876,
            device_name: 'Thermux Lab', bound_ip: '192.168.1.205', objects: 6,
            packets: 4, last_packet_age_s: 2,
        });
        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#bacnet-status')).toHaveText('Running');
        await expect(page.locator('#bacnet-enabled')).toBeChecked();
        await expect(page.locator('#bacnet-port')).toHaveValue('47809');
        await expect(page.locator('#bacnet-device-instance')).toHaveValue('9876');
        await expect(page.locator('#bacnet-device-name')).toHaveValue('Thermux Lab');
        await expect(page.locator('#bacnet-info')).toContainText('Objects: 6 Analog Input');
        await expect(page.locator('#bacnet-info')).toContainText('Packets served: 4');
    });

    test('saving sends enabled, UDP port, device instance and name', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#bacnet-info')).not.toContainText('Loading');

        await page.locator('#bacnet-enabled').check();
        await page.locator('#bacnet-port').fill('47810');
        await page.locator('#bacnet-device-instance').fill('123456');
        await page.locator('#bacnet-device-name').fill('Mechanical Room');
        await page.locator('#bacnet-form button[type="submit"]').click();

        await expect(page.locator('#toast')).toContainText('BACnet/IP settings saved');
        await expect(page.locator('#bacnet-status')).toHaveText('Running');
        const post = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/bacnet');
        expect(post, 'expected a POST /api/bacnet request').toBeTruthy();
        expect(JSON.parse(post.body)).toEqual({ enabled: true, udp_port: 47810, device_instance: 123456, device_name: 'Mechanical Room' });
    });

    test('an out-of-range device instance is blocked without calling the device', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#bacnet-info')).not.toContainText('Loading');

        await page.locator('#bacnet-device-instance').fill('4194303');
        await page.locator('#bacnet-form button[type="submit"]').click();

        expect(await page.locator('#bacnet-device-instance').evaluate((el) => el.validity.valid)).toBe(false);
        await page.waitForTimeout(200);
        expect(mock.requests.some((r) => r.method === 'POST' && r.path === '/api/bacnet')).toBe(false);
    });

    test('a failed restart shows the device error', async ({ page }) => {
        mock = await startMockServer();
        mock.state.bacnetFailMessage = 'Could not apply BACnet/IP settings (ESP_FAIL); previous settings kept';
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#bacnet-info')).not.toContainText('Loading');

        await page.locator('#bacnet-enabled').check();
        await page.locator('#bacnet-form button[type="submit"]').click();

        await expect(page.locator('#toast')).toContainText('previous settings kept');
    });
});
