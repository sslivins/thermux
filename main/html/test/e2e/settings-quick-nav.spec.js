// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the quick-nav shortcut bar at the top of the
 * Settings page, which jumps to each config-section panel via in-page
 * anchor links (#panel-network, #panel-mqtt, etc.).
 */

test.describe('Settings page quick-nav', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('shows a shortcut link for every settings panel', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);

        const nav = page.locator('#quick-nav');
        await expect(nav).toBeVisible();

        const panelIds = [
            'panel-network',
            'panel-mqtt',
            'panel-sensors',
            'panel-firmware',
            'panel-logs',
            'panel-security',
            'panel-backup',
            'panel-system',
        ];
        for (const id of panelIds) {
            await expect(nav.locator(`a[href="#${id}"]`)).toBeVisible();
            await expect(page.locator(`#${id}`)).toBeAttached();
        }
    });

    test('clicking a shortcut scrolls the target panel into view', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);

        await page.locator('#quick-nav a[href="#panel-backup"]').click();

        await expect(page.locator('#panel-backup')).toBeInViewport();
    });
});
