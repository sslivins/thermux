// Packaging smoke check: verifies scripts/gzip_html.py (the exact script
// CMake invokes via EMBED_FILES) produces gzip output that decompresses
// back to byte-identical source HTML. This is the one piece of the real
// firmware serving path (main/web_server.c sets Content-Encoding: gzip
// and serves these embedded blobs verbatim) that the mock-server-based
// Playwright suite does NOT exercise, since it serves plain uncompressed
// HTML for simplicity/speed.
//
// Requires a `python3` (or `python`) interpreter on PATH - same
// requirement as the actual CMake build.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { spawnSync } = require('node:child_process');

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const HTML_DIR = path.join(REPO_ROOT, 'main', 'html');
const GZIP_SCRIPT = path.join(REPO_ROOT, 'scripts', 'gzip_html.py');

function findPython() {
    for (const candidate of ['python3', 'python']) {
        const result = spawnSync(candidate, ['--version']);
        if (result.status === 0) return candidate;
    }
    return null;
}

test('gzip_html.py output decompresses to byte-identical source for each served HTML file', (t) => {
    const python = findPython();
    if (!python) {
        t.skip('no python3/python interpreter on PATH - skipping packaging smoke check');
        return;
    }

    const outDir = fs.mkdtempSync(path.join(os.tmpdir(), 'thermux-gzip-smoke-'));
    try {
        const result = spawnSync(python, [GZIP_SCRIPT, HTML_DIR, outDir], { encoding: 'utf8' });
        assert.equal(result.status, 0, `gzip_html.py failed: ${result.stderr}`);

        for (const filename of ['index.html', 'config.html']) {
            const sourcePath = path.join(HTML_DIR, filename);
            const gzPath = path.join(outDir, `${filename}.gz`);

            assert.ok(fs.existsSync(gzPath), `expected ${gzPath} to be produced`);

            const sourceBytes = fs.readFileSync(sourcePath);
            const decompressed = zlib.gunzipSync(fs.readFileSync(gzPath));

            assert.ok(
                sourceBytes.equals(decompressed),
                `${filename}: decompressed .gz does not byte-match source`
            );
        }
    } finally {
        fs.rmSync(outDir, { recursive: true, force: true });
    }
});
