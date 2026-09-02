import { MariaDBlite } from '../dist/index.mjs';

const db = await MariaDBlite.create();
try {
  const one = db.query('SELECT 1 AS n');
  console.log('SELECT1', JSON.stringify(one));

  db.exec('DROP TABLE IF EXISTS launch_t');
  db.exec(
    'CREATE TABLE launch_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB'
  );
  db.exec("INSERT INTO launch_t VALUES (1, 'launch-ok')");
  const rows = db.query('SELECT v FROM launch_t WHERE id = 1');
  console.log('INNODB', JSON.stringify(rows));

  const engines = db.query('SHOW ENGINES');
  console.log('ENGINES', JSON.stringify(engines));
} finally {
  db.close();
}
