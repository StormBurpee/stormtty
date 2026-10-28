// Diagnostic only: this reproduces a known upstream gap. It is NOT a StormTTY
// compatibility pass and is deliberately separate from the live Session test.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

assert.equal(process.argv.length, 3,
  'Usage: node replay-xterm.cjs /absolute/scratch/node_modules/@xterm/headless');
const packageRoot = process.argv[2];
assert.ok(path.isAbsolute(packageRoot), 'Use an explicit absolute reference package path');
const metadata = JSON.parse(fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8'));
assert.equal(metadata.name, '@xterm/headless');
assert.equal(metadata.version, '6.0.0', 'This diagnosis is qualified against xterm/headless 6.0.0');
const {Terminal} = require(packageRoot);
const trace = JSON.parse(fs.readFileSync(path.join(__dirname, 'trace.json'), 'utf8'));
assert.equal(trace.format, 'stormtty-ink-resize-replay-v1');
function decode(part) {
  const bytes = Buffer.from(part.base64, 'base64');
  assert.equal(bytes.length, part.bytes);
  assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), part.sha256);
  return bytes;
}
const stream = Buffer.concat([decode(trace.before), decode(trace.after)]);

async function replay(label, offset) {
  const term = new Terminal({cols: trace.initial.columns, rows: trace.initial.rows, allowProposedApi: true});
  const write = bytes => new Promise(resolve => term.write(bytes, resolve));
  try {
    await write(stream.subarray(0, offset));
    term.resize(trace.resized.columns, trace.resized.rows);
    await write(stream.subarray(offset));
    assert.equal(term.buffer.active.type, 'alternate');
    const rows = Array.from({length: term.rows}, (_, row) =>
      term.buffer.active.getLine(term.buffer.active.viewportY + row).translateToString(true, 0, term.cols));
    console.log(`${label}: resize at byte ${offset}; ${term.cols}x${term.rows} alternate screen`);
    for (let row = 0; row < rows.length; ++row) console.log(`${row}: ${rows[row]}`);
    return rows;
  } finally {
    term.dispose();
  }
}

(async () => {
  const baseline = await replay('baseline', trace.baseline_resize_offset);
  const inflight = await replay('in-flight old-width animation', trace.inflight_resize_offset);
  assert.ok(baseline[0].startsWith('╭') && baseline[0].endsWith('╮'));
  assert.ok(baseline[1].includes('StormTTY Ink Workbench'));
  assert.ok(inflight[0].startsWith('╭') && !inflight[0].includes('╮'));
  assert.ok(inflight[1].startsWith('╭') && inflight[1].endsWith('╮'));
  assert.ok(inflight[2].includes('StormTTY Ink Workbench'));
  assert.ok(inflight[14].startsWith('╰') && inflight[14].endsWith('╯'));
  console.log('Reproduced persistent compact alternate-screen resize gap in the independent reference parser.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
