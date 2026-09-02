import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MariaDBlite } from '../dist/index.mjs';

test('dumpDataDir/loadDataDir round-trips a database snapshot', async () => {
  const db = await MariaDBlite.create();
  let dump;
  try {
    db.exec('DROP TABLE IF EXISTS snap_t');
    db.exec(
      'CREATE TABLE snap_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
    );
    db.exec("INSERT INTO snap_t VALUES (1, 'snapshot-me')");
    dump = await db.dumpDataDir();
    assert.ok(dump instanceof Uint8Array);
    // gzip magic
    assert.equal(dump[0], 0x1f);
    assert.equal(dump[1], 0x8b);
  } finally {
    await db.close();
  }

  const restored = await MariaDBlite.create({ loadDataDir: dump });
  try {
    const rows = restored.query('SELECT v FROM snap_t WHERE id = 1');
    assert.deepEqual(
      rows.map((r) => r.v),
      ['snapshot-me']
    );
  } finally {
    await restored.close();
  }
});

test('dumpDataDir({compress:false}) produces a plain tar', async () => {
  const db = await MariaDBlite.create();
  try {
    const tar = await db.dumpDataDir({ compress: false });
    // ustar magic at offset 257
    assert.equal(new TextDecoder().decode(tar.subarray(257, 262)), 'ustar');
  } finally {
    await db.close();
  }
});
