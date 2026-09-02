import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MariaDBlite } from '../dist/index.mjs';

test('parameterized queries escape values safely', async () => {
  const db = await MariaDBlite.create();
  try {
    db.exec('DROP TABLE IF EXISTS params_t');
    db.exec(
      'CREATE TABLE params_t (id INT PRIMARY KEY, v VARCHAR(128), b BLOB, n INT) ENGINE=InnoDB'
    );

    const nasty = "a'b\\c\"d\n-- drop; ?";
    db.exec('INSERT INTO params_t VALUES (?, ?, ?, ?)', [
      1,
      nasty,
      new Uint8Array([0, 1, 254, 255]),
      null,
    ]);

    const rows = db.query('SELECT v, b, n FROM params_t WHERE id = ?', [1]);
    assert.equal(rows.length, 1);
    assert.equal(rows[0].v, nasty);
    assert.deepEqual([...rows[0].b], [0, 1, 254, 255]);
    assert.equal(rows[0].n, null);

    // array expansion for IN
    db.exec('INSERT INTO params_t VALUES (?, ?, NULL, NULL)', [2, 'two']);
    db.exec('INSERT INTO params_t VALUES (?, ?, NULL, NULL)', [3, 'three']);
    const inRows = db.query(
      'SELECT v FROM params_t WHERE id IN ? ORDER BY id',
      [[2, 3]]
    );
    assert.deepEqual(
      inRows.map((r) => r.v),
      ['two', 'three']
    );

    // ? inside strings and comments is not a placeholder
    const q = db.query("SELECT '?' AS q, 1 -- ? comment\n", []);
    assert.equal(q[0].q, '?');

    // placeholder/param count mismatches throw
    assert.throws(() => db.query('SELECT ?, ?', [1]), /more placeholders/);
    assert.throws(() => db.query('SELECT ?', [1, 2]), /more parameters/);

    // injection attempt via params must not break out
    const inj = db.query('SELECT v FROM params_t WHERE v = ?', [
      "' OR 1=1 --",
    ]);
    assert.equal(inj.length, 0);
  } finally {
    await db.close();
  }
});
