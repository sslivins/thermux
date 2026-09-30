// @ts-check
const { expect } = require('@playwright/test');

/**
 * Answers the themed confirm dialog (#confirm-dialog) that replaced
 * window.confirm(). Waits for it to open, returns its visible text,
 * presses the confirm or cancel button and waits for the dialog's close
 * event (not for it to be hidden: a follow-up dialog may reopen it).
 *
 * @param {import('@playwright/test').Page} page
 * @param {boolean} accept
 */
async function answerConfirm(page, accept) {
    const dialog = page.locator('#confirm-dialog');
    await expect(dialog).toBeVisible();
    const text = await dialog.innerText();
    const closed = dialog.evaluate((d) => new Promise((resolve) => d.addEventListener('close', resolve, { once: true })));
    await dialog.locator(accept ? '#confirm-dialog-ok' : '#confirm-dialog-cancel').click();
    await closed;
    return text;
}

module.exports = { answerConfirm };
