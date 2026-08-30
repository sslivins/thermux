// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the firmware "Check for Updates" flow on the
 * Settings page, focused on how a pre-release (beta) latest version is
 * labelled. When the beta channel is enabled, /api/ota/status reports
 * latest_is_prerelease=true and the UI should append " (beta)" so it's
 * clear the offered version is not a stable release.
 */

test.describe('OTA latest-version beta labelling', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.afterEach(async () => {
        await mock.close();
    });

    test('labels a pre-release latest version with (beta)', async ({ page }) => {
        mock = await startMockServer();
        mock.state.otaStatus.checking = false;
        mock.state.otaStatus.result = 1;
        mock.state.otaStatus.update_available = true;
        mock.state.otaStatus.current_version = '3.3.6';
        mock.state.otaStatus.latest_version = 'v3.3.8';
        mock.state.otaStatus.latest_is_prerelease = true;

        await page.goto(`${mock.baseURL}/config`);
        await page.locator('#check-btn').click();

        await expect(page.locator('#fw-latest-version')).toHaveText('v3.3.8 (beta)');
        await expect(page.locator('#update-status')).toContainText('Update available: v3.3.8 (beta)');
    });

    test('does not add (beta) for a stable latest version', async ({ page }) => {
        mock = await startMockServer();
        mock.state.otaStatus.checking = false;
        mock.state.otaStatus.result = 1;
        mock.state.otaStatus.update_available = true;
        mock.state.otaStatus.current_version = '3.3.6';
        mock.state.otaStatus.latest_version = 'v3.3.9';
        mock.state.otaStatus.latest_is_prerelease = false;

        await page.goto(`${mock.baseURL}/config`);
        await page.locator('#check-btn').click();

        await expect(page.locator('#fw-latest-version')).toHaveText('v3.3.9');
        await expect(page.locator('#fw-latest-version')).not.toContainText('beta');
    });
});
