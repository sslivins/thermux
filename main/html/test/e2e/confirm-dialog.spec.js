// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');
const { answerConfirm } = require('./confirm-helpers');

/**
 * The themed confirm dialog that replaced window.confirm(). Runs in the
 * desktop and phone projects (see playwright.config.js): a centered card
 * with side-by-side buttons on desktop, a bottom sheet with full-width
 * stacked buttons on phones.
 */

test.describe('Confirm dialog', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;
    let nativeDialogs = 0;

    test.beforeEach(async ({ page }) => {
        mock = await startMockServer();
        nativeDialogs = 0;
        page.on('dialog', (d) => { nativeDialogs++; d.dismiss(); });
        await page.goto(`${mock.baseURL}/config`);
    });

    test.afterEach(async () => {
        await mock.close();
        expect(nativeDialogs, 'no native browser popups').toBe(0);
    });

    const restartPosts = () => mock.requests.filter((r) => r.method === 'POST' && r.path === '/api/system/restart');
    const openRestart = (page) => page.getByRole('button', { name: /Restart Device/ }).click();

    test('shows a themed dialog with a title and custom button labels', async ({ page }) => {
        await openRestart(page);
        const dialog = page.locator('#confirm-dialog');
        await expect(dialog).toBeVisible();
        await expect(dialog.locator('#confirm-dialog-title')).toHaveText('Restart the device?');
        await expect(dialog.locator('#confirm-dialog-ok')).toHaveText('Restart');
        await expect(dialog.locator('#confirm-dialog-cancel')).toHaveText('Cancel');
        await expect(dialog.locator('#confirm-dialog-ok')).toBeFocused();

        await answerConfirm(page, true);
        await expect(page.locator('#toast')).toContainText('Device restarting');
        expect(restartPosts()).toHaveLength(1);
    });

    test('Cancel does nothing', async ({ page }) => {
        await openRestart(page);
        await answerConfirm(page, false);
        await page.waitForTimeout(200);
        expect(restartPosts()).toHaveLength(0);
    });

    test('Escape closes it without doing anything', async ({ page }) => {
        await openRestart(page);
        await expect(page.locator('#confirm-dialog')).toBeVisible();
        await page.keyboard.press('Escape');
        await expect(page.locator('#confirm-dialog')).toBeHidden();
        await page.waitForTimeout(200);
        expect(restartPosts()).toHaveLength(0);
    });

    test('tapping outside the dialog closes it without doing anything', async ({ page }) => {
        await openRestart(page);
        await expect(page.locator('#confirm-dialog')).toBeVisible();
        await page.mouse.click(5, 5);
        await expect(page.locator('#confirm-dialog')).toBeHidden();
        await page.waitForTimeout(200);
        expect(restartPosts()).toHaveLength(0);
    });

    test('clicking inside the dialog does not close it', async ({ page }) => {
        await openRestart(page);
        await page.locator('#confirm-dialog-message').click();
        await expect(page.locator('#confirm-dialog')).toBeVisible();
    });

    test('destructive actions get a red button, focus Cancel, and factory reset asks twice', async ({ page }) => {
        const resetPosts = () => mock.requests.filter((r) => r.method === 'POST' && r.path === '/api/system/factory-reset');
        await page.getByRole('button', { name: /Factory Reset/ }).click();
        const ok = page.locator('#confirm-dialog-ok');
        await expect(ok).toHaveClass(/btn-danger/);
        await expect(page.locator('#confirm-dialog-cancel')).toBeFocused();
        await answerConfirm(page, true);

        await expect(page.locator('#confirm-dialog-title')).toHaveText('Last chance');
        await expect(ok).toHaveText('Erase everything');
        await answerConfirm(page, false);
        await page.waitForTimeout(200);
        expect(resetPosts()).toHaveLength(0);

        await page.getByRole('button', { name: /Factory Reset/ }).click();
        await answerConfirm(page, true);
        await answerConfirm(page, true);
        await expect.poll(() => resetPosts().length).toBe(1);
    });

    test('the page behind cannot be scrolled while it is open', async ({ page }) => {
        await openRestart(page);
        await expect(page.locator('#confirm-dialog')).toBeVisible();
        const overflow = await page.evaluate(() => getComputedStyle(document.documentElement).overflow);
        expect(overflow).toBe('hidden');
    });

    test('layout fits the screen: bottom sheet on phones, centered card on desktop', async ({ page }) => {
        await openRestart(page);
        const vp = /** @type {{width: number, height: number}} */ (page.viewportSize());
        const box = /** @type {NonNullable<Awaited<ReturnType<import('@playwright/test').Locator['boundingBox']>>>} */
            (await page.locator('#confirm-dialog').boundingBox());
        const ok = await page.locator('#confirm-dialog-ok').boundingBox();
        const cancel = await page.locator('#confirm-dialog-cancel').boundingBox();

        expect(box.x).toBeGreaterThanOrEqual(0);
        expect(box.x + box.width).toBeLessThanOrEqual(vp.width);
        expect(box.y + box.height).toBeLessThanOrEqual(vp.height);
        expect(ok.height).toBeGreaterThanOrEqual(44);
        expect(cancel.height).toBeGreaterThanOrEqual(44);

        if (vp.width <= 520) {
            expect(vp.height - (box.y + box.height)).toBeLessThanOrEqual(40);
            expect(ok.y).toBeLessThan(cancel.y);
            expect(Math.abs(ok.width - cancel.width)).toBeLessThan(2);
        } else {
            expect(Math.abs((box.y + box.height / 2) - vp.height / 2)).toBeLessThan(40);
            expect(Math.abs(ok.y - cancel.y)).toBeLessThan(2);
            expect(cancel.x).toBeLessThan(ok.x);
        }
    });
});
