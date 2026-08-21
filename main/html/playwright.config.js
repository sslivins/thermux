// @ts-check
const { defineConfig, devices } = require('@playwright/test');

/**
 * Playwright config for the Thermux embedded web UI.
 *
 * There is no dev server for these files - they're static HTML/JS served
 * directly by the ESP32 firmware in production. For local/CI testing we
 * spin up a minimal Node mock server (test/mock-server.js) per test file
 * that serves the real files at the real routes ("/" and "/config") and
 * implements just enough of the JSON API surface to drive the UI. See
 * test/mock-server.js for what is/isn't faithfully mocked.
 */
module.exports = defineConfig({
    testDir: './test/e2e',
    fullyParallel: true,
    forbidOnly: !!process.env.CI,
    retries: process.env.CI ? 1 : 0,
    reporter: process.env.CI ? 'github' : 'list',
    use: {
        trace: 'on-first-retry',
    },
    projects: [
        { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    ],
});
