// Diagnostic independent-parser replay of bounded synthetic fixture captures.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const crypto = require('node:crypto');
const path = require('node:path');
assert.equal(process.argv.length, 4,
  'Usage: node replay-transport.cjs /absolute/@xterm/headless /absolute/capture-dir');
const [packageRoot, directory] = process.argv.slice(2);
assert.ok(path.isAbsolute(packageRoot) && path.isAbsolute(directory));
const metadata = JSON.parse(fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8'));
assert.equal(metadata.name, '@xterm/headless');
assert.equal(metadata.version, '6.0.0');
const {Terminal} = require(packageRoot);
const unicodeRoot = path.join(path.dirname(packageRoot), 'addon-unicode11');
const unicodeMetadata = JSON.parse(fs.readFileSync(path.join(unicodeRoot, 'package.json'), 'utf8'));
assert.equal(unicodeMetadata.version, '0.9.0');
const {Unicode11Addon} = require(unicodeRoot);
(async () => {
  let failures = 0;
  for (const columns of [56, 72, 73]) {
    for (const kind of ['stdout', 'transport']) {
      const label = `${columns}x20 ${kind}`;
      const term = new Terminal({cols: columns, rows: 20, allowProposedApi: true, convertEol: kind === 'stdout'});
      term.loadAddon(new Unicode11Addon());
      term.unicode.activeVersion = '11';
      try {
        const file = path.join(directory, `${columns}x20-${kind}.bin`);
        assert.ok(fs.statSync(file).size <= 1024 * 1024);
        const bytes = fs.readFileSync(file);
        await new Promise(resolve => term.write(bytes, resolve));
        const buffer = term.buffer.active;
        const rows = Array.from({length: 20}, (_, y) => buffer.getLine(buffer.viewportY + y));
        console.log(`xterm Unicode11 ${label} bytes=${bytes.length} sha256=${crypto.createHash('sha256').update(bytes).digest('hex')} replay=${kind === 'stdout' ? 'LNM/convertEol' : 'byte-exact'}`);
        rows.forEach((row, y) => console.log(`${y}: ${row.translateToString(true)}`));
        assert.equal(buffer.type, 'alternate');
        const width = Math.min(columns, 72);
        assert.equal(rows[0].getCell(0).getChars(), '╭');
        assert.equal(rows[0].getCell(width - 1).getChars(), '╮');
        assert.equal(rows[19].getCell(0).getChars(), '╰');
        assert.equal(rows[19].getCell(width - 1).getChars(), '╯');
        for (let y = 1; y < 19; ++y) {
          assert.equal(rows[y].getCell(0).getChars(), '│', `left border row ${y}`);
          assert.equal(rows[y].getCell(width - 1).getChars(), '│', `right border row ${y}`);
        }
        assert.ok(rows[3].translateToString(true).includes('Unicode: café e\u0301 日本 🚀'));
        console.log(`xterm ${label}: coherent`);
      } catch (error) {
        ++failures;
        console.log(`xterm ${label}: FAIL: ${error.message}`);
      } finally { term.dispose(); }
    }
  }
  process.exitCode = failures ? 1 : 0;
})().catch(error => { console.error(error); process.exitCode = 1; });
