/**
 * Unit tests for pure JS helper functions embedded in main/html/index.html.
 *
 * index.html has no build step / bundler, so we extract the contents of its
 * single <script> tag and evaluate it in a sandboxed Node vm context, then
 * exercise the pure (DOM-free) functions it exposes via `module.exports`.
 *
 * Run with: node --test main/html/test/index.test.js
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function loadIndexHtmlExports() {
    const htmlPath = path.join(__dirname, '..', 'index.html');
    const html = fs.readFileSync(htmlPath, 'utf8');

    const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
    assert.ok(scriptMatch, 'expected to find a <script> block in index.html');
    const scriptSrc = scriptMatch[1];

    const moduleObj = { exports: {} };
    const sandbox = {
        module: moduleObj,
        console,
        // Minimal browser-global stubs so the top-level script body (which
        // calls fetchStatus()/fetchSensors() and sets up a setInterval at
        // load time) can be evaluated without a real DOM/network.
        window: {},
        document: { addEventListener: () => {}, getElementById: () => ({ textContent: '', style: {}, className: '' }) },
        setInterval: () => 0,
        clearInterval: () => {},
        setTimeout: () => 0,
        clearTimeout: () => {},
        fetch: () => Promise.resolve({ ok: false, status: 0, json: () => Promise.resolve({}) }),
    };
    vm.createContext(sandbox);
    vm.runInContext(scriptSrc, sandbox, { filename: 'index.html-inline-script' });

    return moduleObj.exports;
}

test('shouldSaveOnBlur: returns true (save) when not cancelled', () => {
    const { shouldSaveOnBlur } = loadIndexHtmlExports();
    assert.equal(shouldSaveOnBlur(undefined), true);
    assert.equal(shouldSaveOnBlur(''), true);
    assert.equal(shouldSaveOnBlur('0'), true);
});

test('shouldSaveOnBlur: returns false (cancel) when cancelled flag is set', () => {
    const { shouldSaveOnBlur } = loadIndexHtmlExports();
    assert.equal(shouldSaveOnBlur('1'), false);
});
