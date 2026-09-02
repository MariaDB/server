import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MariaDBlite } from '../dist/index.mjs';

test('column values are coerced to JS types', async () => {
  const db = await MariaDBlite.create();
  try {
    db.exec('DROP TABLE IF EXISTS types_t');
    db.exec(`CREATE TABLE types_t (
      i INT,
      bi BIGINT,
      huge BIGINT,
      d DOUBLE,
      dec_val DECIMAL(10,3),
      v VARCHAR(32),
      t TEXT,
      b BLOB,
      dt DATETIME,
      da DATE,
      nullable INT
    ) ENGINE=InnoDB`);
    db.exec(`INSERT INTO types_t VALUES (
      42,
      9007199254740991,
      9223372036854775807,
      2.5,
      3.140,
      'hello',
      'long text',
      X'00FF10',
      '2026-09-02 12:34:56',
      '2026-09-02',
      NULL
    )`);

    const rows = db.query('SELECT * FROM types_t');
    assert.equal(rows.length, 1);
    const r = rows[0];

    assert.equal(r.i, 42);
    assert.equal(typeof r.i, 'number');

    assert.equal(r.bi, 9007199254740991); // safe integer -> number
    assert.equal(r.huge, '9223372036854775807'); // unsafe -> string

    assert.equal(r.d, 2.5);
    assert.equal(r.dec_val, '3.140'); // exact decimal stays string

    assert.equal(r.v, 'hello');
    assert.equal(r.t, 'long text');

    assert.ok(r.b instanceof Uint8Array);
    assert.deepEqual([...r.b], [0, 255, 16]);

    assert.equal(r.dt, '2026-09-02 12:34:56');
    assert.equal(r.da, '2026-09-02');
    assert.equal(r.nullable, null);
  } finally {
    await db.close();
  }
});
