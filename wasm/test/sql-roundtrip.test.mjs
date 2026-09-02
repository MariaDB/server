import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Lite4MariaDB } from '../dist/index.mjs';

test('CREATE / INSERT / SELECT round-trip on shipped API', async () => {
  const db = await Lite4MariaDB.create();
  try {
    db.exec('DROP TABLE IF EXISTS roundtrip');
    db.exec(
      "CREATE TABLE roundtrip (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB"
    );
    db.exec("INSERT INTO roundtrip VALUES (1, 'hello-wasm')");
    const rows = db.query('SELECT v FROM roundtrip WHERE id = 1');
    assert.equal(rows.length, 1);
    assert.equal(rows[0].v, 'hello-wasm');
  } finally {
    db.close();
  }
});
