import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Lite4MariaDB } from '../dist/index.mjs';

test('transaction() commits on success and rolls back on throw', async () => {
  const db = await Lite4MariaDB.create();
  try {
    db.exec('DROP TABLE IF EXISTS tx_t');
    db.exec(
      'CREATE TABLE tx_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
    );

    const ret = await db.transaction(async (tx) => {
      tx.exec('INSERT INTO tx_t VALUES (?, ?)', [1, 'committed']);
      return 'tx-result';
    });
    assert.equal(ret, 'tx-result');
    assert.deepEqual(
      db.query('SELECT v FROM tx_t').map((r) => r.v),
      ['committed']
    );

    await assert.rejects(
      db.transaction(async (tx) => {
        tx.exec('INSERT INTO tx_t VALUES (?, ?)', [2, 'rolled-back']);
        throw new Error('boom');
      }),
      /boom/
    );
    assert.deepEqual(
      db.query('SELECT v FROM tx_t ORDER BY id').map((r) => r.v),
      ['committed']
    );
  } finally {
    await db.close();
  }
});
