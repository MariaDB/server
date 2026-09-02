import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MariaDBlite } from '../dist/index.mjs';

test('execMulti runs a script and returns one result per statement', async () => {
  const db = await MariaDBlite.create();
  try {
    const results = db.execMulti(`
      DROP TABLE IF EXISTS multi_t;
      CREATE TABLE multi_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB;
      INSERT INTO multi_t VALUES (1, 'a'), (2, 'b');
      SELECT v FROM multi_t ORDER BY id;
    `);
    assert.equal(results.length, 4);
    assert.equal(results[1].affected, 0);
    assert.equal(results[2].affected, 2);
    assert.deepEqual(
      results[3].rows.map((r) => r.v),
      ['a', 'b']
    );
  } finally {
    await db.close();
  }
});

test('execMulti stops at the first error', async () => {
  const db = await MariaDBlite.create();
  try {
    db.exec('DROP TABLE IF EXISTS multi_err_t');
    assert.throws(
      () =>
        db.execMulti(`
          CREATE TABLE multi_err_t (id INT PRIMARY KEY) ENGINE=InnoDB;
          INSERT INTO no_such_table VALUES (1);
          INSERT INTO multi_err_t VALUES (1);
        `),
      (err) => err.errno > 0
    );
    // first statement ran, the one after the error did not
    assert.deepEqual(db.query('SELECT * FROM multi_err_t'), []);
  } finally {
    await db.close();
  }
});
