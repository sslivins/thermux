/**
 * Unit tests for pure JS helper functions embedded in main/html/config.html.
 *
 * config.html has no build step / bundler, so we extract the contents of its
 * single <script> tag and evaluate it in a sandboxed Node vm context, then
 * exercise the pure (DOM-free) functions it exposes via `module.exports`.
 *
 * Run with: node --test main/html/test/config.test.js
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function makeStubElement() {
    return {
        textContent: '',
        value: '',
        checked: false,
        disabled: false,
        placeholder: '',
        style: {},
        className: '',
        classList: { add: () => {}, remove: () => {}, toggle: () => {} },
        addEventListener: () => {},
    };
}

function loadConfigHtmlExports() {
    const htmlPath = path.join(__dirname, '..', 'config.html');
    const html = fs.readFileSync(htmlPath, 'utf8');

    const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
    assert.ok(scriptMatch, 'expected to find a <script> block in config.html');
    const scriptSrc = scriptMatch[1];

    const moduleObj = { exports: {} };
    const sandbox = {
        module: moduleObj,
        console,
        // Minimal browser-global stubs so the top-level script body (which
        // calls loadConfig()/loadLogLevel()/loadAuthStatus() and wires up a
        // change listener at load time) can be evaluated without a real
        // DOM/network.
        window: {},
        document: {
            addEventListener: () => {},
            getElementById: () => makeStubElement(),
        },
        setInterval: () => 0,
        clearInterval: () => {},
        setTimeout: () => 0,
        clearTimeout: () => {},
        confirm: () => true,
        fetch: () => Promise.resolve({ ok: false, status: 0, json: () => Promise.resolve({}) }),
    };
    vm.createContext(sandbox);
    vm.runInContext(scriptSrc, sandbox, { filename: 'config.html-inline-script' });

    return moduleObj.exports;
}

test('buildBackupQuery: all sections excluded', () => {
    const { buildBackupQuery } = loadConfigHtmlExports();
    assert.equal(buildBackupQuery(false, false, false), 'mqtt=0&wifi=0&auth=0');
});

test('buildBackupQuery: all sections included', () => {
    const { buildBackupQuery } = loadConfigHtmlExports();
    assert.equal(buildBackupQuery(true, true, true), 'mqtt=1&wifi=1&auth=1');
});

test('buildBackupQuery: matches agreed defaults (mqtt on, wifi/auth off)', () => {
    const { buildBackupQuery } = loadConfigHtmlExports();
    assert.equal(buildBackupQuery(true, false, false), 'mqtt=1&wifi=0&auth=0');
});

test('restoreFileLabelState: no file selected (initial/cleared state)', () => {
    const { restoreFileLabelState } = loadConfigHtmlExports();
    const state = restoreFileLabelState(null);
    assert.equal(state.text, '📄 Choose backup file...');
    assert.equal(state.hasFile, false);
    assert.equal(state.disabled, true);
});

test('restoreFileLabelState: file selected', () => {
    const { restoreFileLabelState } = loadConfigHtmlExports();
    const state = restoreFileLabelState({ name: 'thermux-backup.json' });
    assert.equal(state.text, '📄 thermux-backup.json');
    assert.equal(state.hasFile, true);
    assert.equal(state.disabled, false);
});
