// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the Modbus channel table: it lists assigned channels,
 * moves (with a swap confirmation when the target is used) and only offers
 * Release for sensors that are no longer connected.
 */

function channel(n, address, extra = {}) {
    return {
        channel: n, temp_register: 100 + n, address, name: null,
        present: true, status: 'ok', temperature: 21.5, ...extra,
    };
}

test.describe('Modbus channel table', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    function row(page, n) {
        return page.locator(`#modbus-channels tr[data-channel="${n}"]`);
    }

    test('lists assigned channels with register, name, status and temperature', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [
            channel(0, '28FF000000000001', { name: 'Supply' }),
            channel(4, '28FF000000000002', { present: false, status: 'missing', temperature: null, name: 'Old probe' }),
        ];
        await page.goto(`${mock.baseURL}/config`);

        await expect(row(page, 0)).toContainText('Supply');
        await expect(row(page, 0)).toContainText('28FF000000000001');
        await expect(row(page, 0)).toContainText('100');
        await expect(row(page, 0)).toContainText('OK');
        await expect(row(page, 0)).toContainText('21.50°C');
        await expect(row(page, 4)).toContainText('104');
        await expect(row(page, 4)).toContainText('Missing');
        await expect(row(page, 4)).toContainText('—');
    });

    test('shows a message when no channels are assigned', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
        await expect(page.locator('#modbus-channels')).toContainText('No sensors have a channel yet');
    });

    test('Release is only offered for missing sensors', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [
            channel(0, '28FF000000000001'),
            channel(1, '28FF000000000002', { present: false, status: 'missing', temperature: null }),
        ];
        await page.goto(`${mock.baseURL}/config`);

        await expect(row(page, 0).locator('.mb-channel-release')).toHaveCount(0);
        await expect(row(page, 1).locator('.mb-channel-release')).toHaveCount(1);
    });

    test('releasing a missing sensor asks first, then removes the row', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [
            channel(1, '28FF000000000002', { present: false, status: 'missing', temperature: null }),
        ];
        await page.goto(`${mock.baseURL}/config`);

        let dialogMessage = '';
        page.once('dialog', (d) => { dialogMessage = d.message(); d.accept(); });
        await row(page, 1).locator('.mb-channel-release').click();

        await expect(page.locator('#toast')).toContainText('Released channel 1');
        await expect(row(page, 1)).toHaveCount(0);
        expect(dialogMessage).toContain('Release channel 1');
        const post = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/modbus/channels');
        expect(JSON.parse(post.body)).toEqual({ action: 'release', channel: 1 });
    });

    test('moving to a free channel sends the move without asking', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [channel(0, '28FF000000000001', { name: 'Supply' })];
        await page.goto(`${mock.baseURL}/config`);

        let asked = false;
        page.on('dialog', (d) => { asked = true; d.accept(); });
        await row(page, 0).locator('.mb-channel-move-input').fill('10');
        await row(page, 0).locator('.mb-channel-move').click();

        await expect(page.locator('#toast')).toContainText('Moved channel 0 to channel 10');
        await expect(row(page, 10)).toContainText('Supply');
        await expect(row(page, 10)).toContainText('110');
        expect(asked).toBe(false);
        const post = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/modbus/channels');
        expect(JSON.parse(post.body)).toEqual({ action: 'move', from: 0, to: 10 });
    });

    test('moving onto a used channel asks before swapping', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [
            channel(0, '28FF000000000001', { name: 'Supply' }),
            channel(1, '28FF000000000002', { name: 'Return' }),
        ];
        await page.goto(`${mock.baseURL}/config`);

        let dialogMessage = '';
        page.once('dialog', (d) => { dialogMessage = d.message(); d.accept(); });
        await row(page, 0).locator('.mb-channel-move-input').fill('1');
        await row(page, 0).locator('.mb-channel-move').click();

        await expect(page.locator('#toast')).toContainText('Moved channel 0 to channel 1');
        expect(dialogMessage).toContain('"Return"');
        await expect(row(page, 0)).toContainText('Return');
        await expect(row(page, 1)).toContainText('Supply');
    });

    test('declining the swap sends nothing', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [channel(0, '28FF000000000001'), channel(1, '28FF000000000002')];
        await page.goto(`${mock.baseURL}/config`);

        page.once('dialog', (d) => d.dismiss());
        await row(page, 0).locator('.mb-channel-move-input').fill('1');
        await row(page, 0).locator('.mb-channel-move').click();

        await page.waitForTimeout(200);
        expect(mock.requests.some((r) => r.method === 'POST' && r.path === '/api/modbus/channels')).toBe(false);
    });

    test('an out-of-range target is rejected without calling the device', async ({ page }) => {
        mock = await startMockServer();
        mock.state.modbusChannels = [channel(0, '28FF000000000001')];
        await page.goto(`${mock.baseURL}/config`);

        await row(page, 0).locator('.mb-channel-move-input').fill('100');
        await row(page, 0).locator('.mb-channel-move').click();

        await expect(page.locator('#toast')).toContainText('between 0 and 99');
        expect(mock.requests.some((r) => r.method === 'POST' && r.path === '/api/modbus/channels')).toBe(false);
    });
});
