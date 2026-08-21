// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Browser-level tests for the "Backup / Restore" feature in config.html.
 *
 * These exercise real DOM interactions (clicks, checkbox toggles, file
 * upload, native confirm() dialog, download event) against the actual
 * config.html served by a minimal mock server (see ../mock-server.js).
 *
 * Scope: this suite verifies the DOM/JS glue wires up correctly - the right
 * endpoints get called with the right params, the right UI feedback shows.
 * It intentionally does NOT re-verify backend business logic (that's
 * covered by the C unit tests in test/) or re-verify the pure JS helpers
 * (buildBackupQuery/restoreFileLabelState - covered by
 * main/html/test/config.test.js).
 */

test.describe('Backup / Restore modal', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.beforeEach(async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/config`);
    });

    test.afterEach(async () => {
        await mock.close();
    });

    test('modal is hidden until "Create Backup" is clicked, then shows agreed default checkbox states', async ({ page }) => {
        const overlay = page.locator('#backup-modal-overlay');
        await expect(overlay).toBeHidden();

        await page.getByRole('button', { name: 'Create Backup' }).click();

        await expect(overlay).toBeVisible();
        // Agreed defaults: MQTT on, WiFi/auth off (see checkpoint 006 design discussion).
        await expect(page.locator('#backup-include-mqtt')).toBeChecked();
        await expect(page.locator('#backup-include-wifi')).not.toBeChecked();
        await expect(page.locator('#backup-include-auth')).not.toBeChecked();
    });

    test('Cancel closes the modal without making a backup request', async ({ page }) => {
        await page.getByRole('button', { name: 'Create Backup' }).click();
        await expect(page.locator('#backup-modal-overlay')).toBeVisible();

        await page.getByRole('button', { name: 'Cancel' }).click();

        await expect(page.locator('#backup-modal-overlay')).toBeHidden();
        expect(mock.requests.some((r) => r.path === '/api/backup')).toBe(false);
    });

    test('Download sends the expected query string for default checkbox states', async ({ page }) => {
        await page.getByRole('button', { name: 'Create Backup' }).click();

        const [download] = await Promise.all([
            page.waitForEvent('download'),
            page.locator('#backup-modal-overlay').getByRole('button', { name: 'Download' }).click(),
        ]);

        const backupRequest = mock.requests.find((r) => r.path === '/api/backup');
        expect(backupRequest, 'expected a GET /api/backup request').toBeTruthy();
        expect(backupRequest.query.get('mqtt')).toBe('1');
        expect(backupRequest.query.get('wifi')).toBe('0');
        expect(backupRequest.query.get('auth')).toBe('0');

        expect(download.suggestedFilename()).toBe('thermux-backup.json');
        await expect(page.locator('#toast')).toContainText('Backup downloaded');
    });

    test('Download reflects toggled checkboxes (wifi + auth included, mqtt excluded)', async ({ page }) => {
        await page.getByRole('button', { name: 'Create Backup' }).click();
        await page.locator('#backup-include-mqtt').uncheck();
        await page.locator('#backup-include-wifi').check();
        await page.locator('#backup-include-auth').check();

        const [download] = await Promise.all([
            page.waitForEvent('download'),
            page.locator('#backup-modal-overlay').getByRole('button', { name: 'Download' }).click(),
        ]);
        void download;

        const backupRequest = mock.requests.find((r) => r.path === '/api/backup');
        expect(backupRequest.query.get('mqtt')).toBe('0');
        expect(backupRequest.query.get('wifi')).toBe('1');
        expect(backupRequest.query.get('auth')).toBe('1');
    });

    test('Restore button stays disabled until a file is chosen', async ({ page }) => {
        const restoreBtn = page.locator('#restore-btn');
        await expect(restoreBtn).toBeDisabled();

        await page.setInputFiles('#restore-file-input', {
            name: 'thermux-backup.json',
            mimeType: 'application/json',
            buffer: Buffer.from(JSON.stringify({ schema_version: 1, sensor_names: [], sensor_settings: {} })),
        });

        await expect(restoreBtn).toBeEnabled();
        await expect(page.locator('#restore-file-label')).toHaveText('📄 thermux-backup.json');
    });

    test('Restore prompts for confirmation, then POSTs the file contents and shows success', async ({ page }) => {
        const backupContents = {
            schema_version: 1,
            device_version: '3.0.0',
            sensor_names: [{ serial: '0102030405aa', name: 'Living Room' }],
            sensor_settings: { read_interval_ms: 10000, publish_interval_ms: 30000, resolution: 12 },
            mqtt: { broker_uri: 'mqtt://test-broker.local:1883', username: 'testuser', password: 'testpass123' },
        };

        await page.setInputFiles('#restore-file-input', {
            name: 'thermux-backup.json',
            mimeType: 'application/json',
            buffer: Buffer.from(JSON.stringify(backupContents)),
        });

        let dialogMessage = '';
        page.once('dialog', (dialog) => {
            dialogMessage = dialog.message();
            dialog.accept();
        });

        await page.locator('#restore-btn').click();

        await expect(page.locator('#toast')).toContainText('Restore complete. Device restarting...');
        expect(dialogMessage).toContain('restart the device');

        const restoreRequest = mock.requests.find((r) => r.method === 'POST' && r.path === '/api/backup/restore');
        expect(restoreRequest, 'expected a POST /api/backup/restore request').toBeTruthy();
        expect(JSON.parse(restoreRequest.body)).toEqual(backupContents);
    });

    test('Restore is aborted if the confirmation dialog is dismissed', async ({ page }) => {
        await page.setInputFiles('#restore-file-input', {
            name: 'thermux-backup.json',
            mimeType: 'application/json',
            buffer: Buffer.from(JSON.stringify({ schema_version: 1, sensor_names: [], sensor_settings: {} })),
        });

        page.once('dialog', (dialog) => dialog.dismiss());
        await page.locator('#restore-btn').click();

        // Give any (incorrect) fetch a moment to fire before asserting it didn't.
        await page.waitForTimeout(200);
        expect(mock.requests.some((r) => r.method === 'POST' && r.path === '/api/backup/restore')).toBe(false);
    });

    test('Restore shows an error toast when the server rejects the backup', async ({ page }) => {
        await page.setInputFiles('#restore-file-input', {
            name: 'bad-backup.json',
            mimeType: 'application/json',
            buffer: Buffer.from('{"not": "valid backup", '), // truncated/invalid JSON
        });

        page.once('dialog', (dialog) => dialog.accept());
        await page.locator('#restore-btn').click();

        await expect(page.locator('#toast')).toContainText('invalid backup file');
    });
});
