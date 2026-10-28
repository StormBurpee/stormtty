import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

test('a piped CI render cannot pass as the interactive compatibility fixture', () => {
  const child = spawnSync(process.execPath, [fileURLToPath(new URL('./fixture.mjs', import.meta.url))], {
    encoding: 'utf8',
    env: {...process.env, CI: 'true'},
    stdio: ['pipe', 'pipe', 'pipe'],
    timeout: 10_000,
    maxBuffer: 128 * 1024,
  });
  assert.equal(child.error, undefined);
  assert.equal(child.signal, null);
  assert.equal(child.status, 1);
  assert.match(child.stderr, /requires a real input\/output TTY/);
  assert.equal(child.stdout, '');
});
