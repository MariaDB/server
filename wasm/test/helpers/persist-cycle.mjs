// Helper run in a child process by persist-nodefs.test.mjs.
// Usage: node persist-cycle.mjs <datadir> <write|read>
import { Lite4MariaDB } from '../../dist/index.mjs';

const [dir, phase] = process.argv.slice(2);

const db = await Lite4MariaDB.create({ dataDir: dir });
try {
  if (phase === 'write') {
    db.exec('DROP TABLE IF EXISTS persist_t');
    db.exec(
      'CREATE TABLE persist_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
    );
    db.exec("INSERT INTO persist_t VALUES (1, 'survived-restart')");
    console.log('WRITE-OK');
  } else {
    const rows = db.query('SELECT v FROM persist_t WHERE id = 1');
    console.log('READ', JSON.stringify(rows));
    if (rows[0]?.v !== 'survived-restart') {
      process.exitCode = 1;
    } else {
      console.log('READ-OK');
    }
  }
} finally {
  await db.close();
}
