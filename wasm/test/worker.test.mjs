import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MariaDBliteWorker } from '../dist/worker.mjs';

test('worker hosts the database off the main thread', async () => {
  const db = await MariaDBliteWorker.create();
  try {
    await db.exec('DROP TABLE IF EXISTS worker_t');
    await db.exec(
      'CREATE TABLE worker_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
    );
    await db.exec('INSERT INTO worker_t VALUES (?, ?)', [1, 'via-worker']);
    const rows = await db.query('SELECT v FROM worker_t WHERE id = ?', [1]);
    assert.deepEqual(
      rows.map((r) => r.v),
      ['via-worker']
    );

    await db.transaction(async (tx) => {
      await tx.exec('INSERT INTO worker_t VALUES (?, ?)', [2, 'tx-worker']);
    });
    const all = await db.query('SELECT v FROM worker_t ORDER BY id');
    assert.deepEqual(
      all.map((r) => r.v),
      ['via-worker', 'tx-worker']
    );

    await assert.rejects(db.query('SELECT * FROM nope_missing'), (err) => {
      assert.ok(err.errno > 0);
      return true;
    });
  } finally {
    await db.close();
  }
});
