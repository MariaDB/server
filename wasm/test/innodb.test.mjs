import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Lite4MariaDB } from '../dist/index.mjs';

test('InnoDB ENGINE stores rows and ROLLBACK hides them', async () => {
  const db = await Lite4MariaDB.create();
  try {
    db.exec('DROP TABLE IF EXISTS innodb_t');
    db.exec(
      'CREATE TABLE innodb_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
    );
    db.exec("INSERT INTO innodb_t VALUES (1, 'committed')");
    const afterInsert = db.query('SELECT v FROM innodb_t ORDER BY id');
    assert.deepEqual(
      afterInsert.map((r) => r.v),
      ['committed']
    );

    db.exec('BEGIN');
    db.exec("INSERT INTO innodb_t VALUES (2, 'rolled-back')");
    const inTxn = db.query('SELECT v FROM innodb_t ORDER BY id');
    assert.deepEqual(
      inTxn.map((r) => r.v),
      ['committed', 'rolled-back']
    );
    db.exec('ROLLBACK');
    const afterRollback = db.query('SELECT v FROM innodb_t ORDER BY id');
    assert.deepEqual(
      afterRollback.map((r) => r.v),
      ['committed']
    );

    const engines = db.query('SHOW ENGINES');
    const innodb = engines.find(
      (r) => String(r.Engine || r.ENGINE || r.engine).toLowerCase() === 'innodb'
    );
    assert.ok(innodb, 'SHOW ENGINES must list InnoDB');
    const support = String(
      innodb.Support || innodb.SUPPORT || innodb.support || ''
    ).toUpperCase();
    assert.notEqual(support, 'DISABLED');
    assert.ok(
      support === 'YES' || support === 'DEFAULT',
      `InnoDB Support should be YES or DEFAULT, got ${support}`
    );
  } finally {
    db.close();
  }
});
