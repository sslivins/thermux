// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the "Include pre-releases (beta channel)"
 * checkbox in config.html's Firmware Update > Cloud tab.
 *
 * Scope: verifies the checkbox reflects the persisted /api/ota/channel
 * value on load, and that toggling it POSTs the new value. Does NOT
 * re-verify GitHub release-list parsing/selection (that lives in
 * ota_updater.c and isn't host-testable - it's tightly coupled to the
 * live HTTPS client and GitHub API response shape).
 */

test.describe('OTA pre-release channel checkbox', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('defaults to unchecked when the device reports the channel disabled', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#ota-include-prerelease')).not.toBeChecked();
    });

    test('reflects an already-enabled channel on load', async ({ page }) => {
        mock = await startMockServer();
        mock.state.otaChannel.include_prerelease = true;
        await page.goto(`${mock.baseURL}/config`);

        await expect(page.locator('#ota-include-prerelease')).toBeChecked();
    });

    test('checking the box POSTs include_prerelease=true and shows a confirmation toast', async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);

        await page.locator('#ota-include-prerelease').check();

        const channelRequest = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/ota/channel');
        expect(channelRequest, 'expected a POST /api/ota/channel request').toBeTruthy();
        expect(JSON.parse(channelRequest.body)).toEqual({ include_prerelease: true });
        await expect(page.locator('#toast')).toContainText('Beta channel enabled');
    });

    test('unchecking the box POSTs include_prerelease=false', async ({ page }) => {
        mock = await startMockServer();
        mock.state.otaChannel.include_prerelease = true;
        await page.goto(`${mock.baseURL}/config`);

        await page.locator('#ota-include-prerelease').uncheck();

        const channelRequest = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/ota/channel');
        expect(JSON.parse(channelRequest.body)).toEqual({ include_prerelease: false });
        await expect(page.locator('#toast')).toContainText('Beta channel disabled');
    });
});
