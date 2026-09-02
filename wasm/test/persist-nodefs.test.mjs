import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const here = dirname(fileURLToPath(import.meta.url));
const helper = join(here, 'helpers', 'persist-cycle.mjs');

// Two separate node processes: the second must see the first's InnoDB data,
// proving the dataDir lives on the real local disk (NODEFS mount).
test('nodefs dataDir persists across process restarts', () => {
  const dir = mkdtempSync(join(tmpdir(), 'lite4mariadb-'));
  try {
    const w = spawnSync(process.execPath, [helper, dir, 'write'], {
      encoding: 'utf8',
      timeout: 180000,
    });
    assert.equal(w.status, 0, `write phase failed: ${w.stderr}`);
    assert.match(w.stdout, /WRITE-OK/);
    assert.ok(existsSync(join(dir, 'test')), 'datadir has test db on disk');

    const r = spawnSync(process.execPath, [helper, dir, 'read'], {
      encoding: 'utf8',
      timeout: 180000,
    });
    assert.equal(r.status, 0, `read phase failed: ${r.stderr}`);
    assert.match(r.stdout, /survived-restart/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
