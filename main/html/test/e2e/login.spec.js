// @ts-check
const { test, expect } = require('@playwright/test');
const { startMockServer } = require('../mock-server');

/**
 * Login page error feedback. A repeated wrong password must look like a new
 * result, not a stale message: the error hides as soon as the user types,
 * and every failure replays a short shake (or a colour flash when the user
 * prefers reduced motion).
 */

test.describe('Login page error feedback', () => {
    /** @type {Awaited<ReturnType<typeof startMockServer>>} */
    let mock;

    test.beforeEach(async ({ page }) => {
        mock = await startMockServer();
        await page.goto(`${mock.baseURL}/login`);
        await page.evaluate(() => {
            /** @type {any} */ (window).errorAnimations = 0;
            document.getElementById('error')?.addEventListener('animationstart', () => {
                /** @type {any} */ (window).errorAnimations++;
            });
        });
    });

    test.afterEach(async () => {
        await mock.close();
    });

    /** @param {import('@playwright/test').Page} page @param {string} pw */
    async function signIn(page, pw) {
        await page.locator('#username').fill('admin');
        await page.locator('#password').fill(pw);
        await page.locator('#submitBtn').click();
    }

    test('a wrong password shows the error, clears the password and refocuses it', async ({ page }) => {
        await signIn(page, 'wrong');

        const error = page.getByRole('alert');
        await expect(error).toBeVisible();
        await expect(error).toHaveText('Invalid username or password');
        await expect(page.locator('#password')).toHaveValue('');
        await expect(page.locator('#password')).toBeFocused();
        await expect(page.locator('#submitBtn')).toBeEnabled();
    });

    test('typing hides the error until the next attempt', async ({ page }) => {
        await signIn(page, 'wrong');
        await expect(page.getByRole('alert')).toBeVisible();

        await page.locator('#password').pressSequentially('x');
        await expect(page.locator('#error')).toBeHidden();
    });

    test('every failed attempt replays the shake', async ({ page }) => {
        await signIn(page, 'wrong');
        await expect(page.getByRole('alert')).toBeVisible();
        await expect.poll(() => page.evaluate(() => /** @type {any} */ (window).errorAnimations)).toBe(1);

        await signIn(page, 'still-wrong');
        await expect(page.getByRole('alert')).toBeVisible();
        await expect.poll(() => page.evaluate(() => /** @type {any} */ (window).errorAnimations)).toBe(2);
        expect(await page.locator('#error').evaluate((el) => getComputedStyle(el).animationName)).toBe('shake');
    });

    test('uses a colour flash instead of a shake when reduced motion is preferred', async ({ page }) => {
        await page.emulateMedia({ reducedMotion: 'reduce' });
        await signIn(page, 'wrong');

        await expect(page.getByRole('alert')).toBeVisible();
        expect(await page.locator('#error').evaluate((el) => getComputedStyle(el).animationName)).toBe('flash');
    });

    test('says when the device cannot be reached', async ({ page }) => {
        mock.state.login.unreachable = true;
        await signIn(page, 'whatever');

        await expect(page.getByRole('alert')).toHaveText("Couldn't reach the device. Try again.");
    });

    test('the right password goes to the dashboard', async ({ page }) => {
        await signIn(page, mock.state.login.password);

        await expect(page).toHaveURL(`${mock.baseURL}/`);
    });
});
