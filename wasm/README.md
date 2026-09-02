# lite4mariadb

MariaDB Server (with InnoDB) compiled to WebAssembly — run **real MariaDB**
in the browser and Node.js with a PGlite-style API. No server process, no
native dependencies: a single `.wasm` module plus a thin JS wrapper.

```bash
npm install lite4mariadb
```

```js
import { Lite4MariaDB } from 'lite4mariadb';

const db = await Lite4MariaDB.create({ dataDir: './my-data' });

db.exec('CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB');
db.exec('INSERT INTO users VALUES (?, ?)', [1, "O'Brien"]);

db.query('SELECT * FROM users WHERE id = ?', [1]);
// => [ { id: 1, name: "O'Brien" } ]

await db.close();
```

## Storage backends

```js
await Lite4MariaDB.create();                              // in-memory (default)
await Lite4MariaDB.create({ dataDir: './my-data' });      // Node.js: real directory
await Lite4MariaDB.create({ dataDir: 'idb://my-app' });   // browser: IndexedDB
```

Reopening the same `dataDir` resumes the database (InnoDB recovery included).
With `idb://`, flushes are debounced automatically — call `await db.persist()`
for a hard flush and always `await db.close()` at the end.

## API

| Method | Returns | Description |
| --- | --- | --- |
| `query(sql, params?)` | `object[]` | Rows with coerced JS types |
| `exec(sql, params?)` | `{ ok, affected, rows, fields, types }` | Full result |
| `execMulti(sql)` | `result[]` | Multi-statement script |
| `transaction(cb)` | callback value | BEGIN/COMMIT, ROLLBACK on throw |
| `persist()` | `Promise<void>` | Flush to IndexedDB (idb://) |
| `dumpDataDir({ compress? }?)` | `Promise<Uint8Array>` | gzipped tar snapshot |
| `close()` | `Promise<void>` | Shutdown |

`Lite4MariaDB.create({ loadDataDir: dump })` restores a snapshot into a fresh
instance — handy for shipping a prebuilt database or persisting `memory://`
databases yourself.

Parameters can be `null`, numbers, bigints, booleans, strings, `Date`,
`Uint8Array`/`ArrayBuffer` (bound as binary), or arrays (expanded for
`IN ?`). Column values are coerced: integers/floats → `number`, unsafe
BIGINT → `string`, DECIMAL → `string`, binary → `Uint8Array`, dates stay
strings.

## Running in a worker (recommended for browsers)

```js
import { Lite4MariaDBWorker } from 'lite4mariadb/worker';

const db = await Lite4MariaDBWorker.create({ dataDir: 'idb://my-app' });
const rows = await db.query('SELECT * FROM users');
await db.close();
```

Same API, fully async; also works with Node `worker_threads`. The build uses
pthreads, so browsers require cross-origin isolation headers
(`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`).

## Notes

* One database instance per process/worker; use extra workers for more.
* Everything is real MariaDB 13.1: InnoDB transactions, foreign keys, window
  functions, CTEs, JSON functions, native vector search.
* Licensed GPL-2.0-only (it is MariaDB). See COPYING.

Source, build instructions and issue tracker:
[github.com/shyim/lite4mariadb](https://github.com/shyim/lite4mariadb)
