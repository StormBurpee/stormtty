const assert = require('node:assert/strict');
const path = require('node:path');
assert.equal(process.argv.length, 3);
assert.ok(path.isAbsolute(process.argv[2]));
const metadata = require(path.join(process.argv[2], 'package.json'));
assert.equal(metadata.name, '@xterm/headless'); assert.equal(metadata.version, '6.0.0');
const {Terminal} = require(process.argv[2]);
(async () => {
  for (const [cols, bytes, expected] of [
    [4, 'ABCD\x1b[K', ['ABCD', '    ']],
    [4, 'ABCD\x1b[0K', ['ABCD', '    ']],
    [4, 'ABCD', ['ABCD', '    ']],
    [5, 'ABCD\x1b[K', ['ABCD ', '     ']],
    [4, 'ABCD\x1b[KZ', ['ABCD', 'Z   ']],
  ]) {
    const term = new Terminal({cols, rows: 2, allowProposedApi: true});
    try {
      await new Promise(resolve => term.write(bytes, resolve));
      const rows = [0, 1].map(y => term.buffer.active.getLine(y).translateToString(false));
      console.log(JSON.stringify({cols, bytes, rows})); assert.deepEqual(rows, expected);
    } finally { term.dispose(); }
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
