# mariadblite

MariaDB Server (with InnoDB) compiled to WebAssembly — a full MariaDB
embedded server that runs in the browser and in Node.js, with a PGlite-style
JavaScript API.

This is a fork of [MariaDB/server](https://github.com/MariaDB/server) that
adds an Emscripten build target and a small JS package around
`libmariadbd` (the embedded server). You get real MariaDB semantics —
InnoDB transactions, foreign keys, window functions, CTEs, JSON functions,
native vector search — without any server process.

```js
import { MariaDBlite } from 'mariadblite';

const db = await MariaDBlite.create({ dataDir: './my-data' });

db.exec('CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB');
db.exec('INSERT INTO users VALUES (?, ?)', [1, "O'Brien"]);

const rows = db.query('SELECT * FROM users WHERE id = ?', [1]);
// => [ { id: 1, name: "O'Brien" } ]

await db.close();
```

## Features

* **Real MariaDB + InnoDB**, single WASM module (~17 MB), no server needed
* **Persistent storage**: real directories in Node.js, IndexedDB in the
  browser, or in-memory
* **Parameterized queries**, **transactions**, **multi-statement scripts**
* **Type coercion** — numbers, bigints, `Uint8Array` for binary, not just
  strings
* **Snapshots** — `dumpDataDir()` / `loadDataDir` as gzipped tarballs
* **Worker support** — run the database off the browser main thread

## Building

Requires [emsdk](https://emscripten.org/docs/getting_started/downloads.html),
CMake and Node.js. The build compiles native host tools first, then the WASM
module:

```bash
git submodule update --init extra/wolfssl/wolfssl libmariadb
./wasm/build.sh        # artifacts land in wasm/dist/
cd wasm && npm test    # run the test suite
```

`wasm/dist/` contains the publishable package: `mariadblite.wasm`,
`mariadblite.js`, `index.mjs`, `worker.mjs`, `worker-entry.mjs`.

## Usage

### Creating a database

```js
// Ephemeral in-memory database (default)
const db = await MariaDBlite.create();

// Node.js: persist to a real directory on disk
const db = await MariaDBlite.create({ dataDir: './my-data' });
const db = await MariaDBlite.create({ dataDir: 'file:///absolute/path' });

// Browser: persist to IndexedDB (one IndexedDB database per name)
const db = await MariaDBlite.create({ dataDir: 'idb://my-app' });
```

A datadir that already contains a database is resumed (InnoDB recovery runs
on open), so reopening the same `dataDir` gives you your data back.

### Queries

```js
// query() returns an array of row objects
const rows = db.query('SELECT * FROM users WHERE age > ?', [30]);

// exec() returns the full result, including affected rows
const res = db.exec('UPDATE users SET age = ? WHERE id = ?', [31, 1]);
// => { ok: true, affected: 1, rows: [] }

// execMulti() runs a whole script, one result per statement
const results = db.execMulti(`
  CREATE TABLE t (id INT PRIMARY KEY) ENGINE=InnoDB;
  INSERT INTO t VALUES (1), (2);
  SELECT COUNT(*) AS n FROM t;
`);
// results[2].rows => [ { n: 2 } ]
```

Parameters may be `null`, numbers, bigints, booleans, strings, `Date`,
`Uint8Array`/`ArrayBuffer` (bound as binary), or arrays (expanded for
`IN ?`). Placeholders inside string literals and comments are ignored.

### Type coercion

Values are converted from the wire format using column metadata:

| MariaDB type                        | JS type                         |
| ----------------------------------- | ------------------------------- |
| INT, TINYINT, SMALLINT, …, YEAR     | `number`                        |
| BIGINT                              | `number`, or `string` if > 2^53 |
| FLOAT, DOUBLE                       | `number`                        |
| DECIMAL                             | `string` (exact)                |
| DATE, DATETIME, TIMESTAMP, TIME     | `string` (timezone-safe)        |
| CHAR, VARCHAR, TEXT, ENUM, SET      | `string`                        |
| BINARY, VARBINARY, BLOB, BIT        | `Uint8Array`                    |
| NULL                                | `null`                          |

### Transactions

```js
await db.transaction(async (tx) => {
  tx.exec('INSERT INTO orders (id, total) VALUES (?, ?)', [1, 99.5]);
  tx.exec('UPDATE stock SET n = n - 1 WHERE id = ?', [42]);
});
// COMMIT on success, ROLLBACK if the callback throws
```

### Snapshots

```js
// Gzipped tar of the datadir as Uint8Array — works even for memory://
const dump = await db.dumpDataDir();                 // or { compress: false }

// Restore into a fresh instance (datadir must be empty)
const db2 = await MariaDBlite.create({ loadDataDir: dump });
```

A live dump is crash-recoverable via InnoDB redo logs; `await db.close()`
first for a clean snapshot.

### Persistence semantics

* **Node (`file://`)** — writes go straight to the host filesystem.
* **Browser (`idb://`)** — IndexedDB is an in-memory overlay: flushes are
  debounced after each statement. Use `await db.persist()` for a hard flush
  and always `await db.close()` at the end.

### Running in a worker

Heavy queries block the thread the module runs on. In the browser, host the
database in a dedicated worker — same API, all methods async:

```js
import { MariaDBliteWorker } from 'mariadblite/worker';

const db = await MariaDBliteWorker.create({ dataDir: 'idb://my-app' });
const rows = await db.query('SELECT * FROM users');
await db.close();
```

Works in Node (`worker_threads`) too. To customize the worker (bundlers,
`locateFile`), pass your own `Worker` running `dist/worker-entry.mjs` as the
second argument to `create()`.

### Browser requirements

The build uses pthreads, which require `SharedArrayBuffer`. Serve your app
with cross-origin isolation headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

## API reference

### `MariaDBlite.create(options?)` → `Promise<MariaDBlite>`

| Option        | Description                                                        |
| ------------- | ------------------------------------------------------------------ |
| `dataDir`     | `memory://` (default), `file://`/bare path (Node), `idb://` (browser) |
| `loadDataDir` | `Uint8Array`/`ArrayBuffer`/`Blob` tarball from `dumpDataDir()`     |
| `locateFile`  | Custom resolver for the `.wasm` asset                              |

### Instance methods

| Method                     | Returns                          | Description                              |
| -------------------------- | -------------------------------- | ---------------------------------------- |
| `query(sql, params?)`      | `Array<object>`                  | Rows; coerced types                      |
| `exec(sql, params?)`       | `{ok, affected, rows, fields…}`  | Full result                              |
| `execMulti(sql)`           | `Array<result>`                  | Multi-statement script                   |
| `transaction(cb)`          | callback's return value          | BEGIN/COMMIT, ROLLBACK on throw          |
| `persist()`                | `Promise<void>`                  | Flush to IndexedDB (idb only)            |
| `dumpDataDir({compress}?)` | `Promise<Uint8Array>`            | gzipped tar snapshot                     |
| `close()`                  | `Promise<void>`                  | Shut down (flushes idb first)            |

Errors are thrown with `errno` and `sqlstate` properties from the server.

## Current limitations

* One database instance per process/worker (the embedded server has global
  state) — open additional instances in separate workers.
* Browser persistence is IDBFS (in-memory overlay flushed to IndexedDB);
  there is no OPFS backend yet.
* SQL errors interrupt `execMulti` scripts at the failing statement.

## Repository layout

* `wasm/` — the JS package: C glue (`mariadblite.c`), JS wrapper
  (`src/`), tests (`test/`), build script (`build.sh`)
* `cmake/os/Emscripten.cmake` — Emscripten platform configuration
* Everything else is upstream [MariaDB Server](https://github.com/MariaDB/server),
  with small Emscripten portability patches.

## Licensing

MariaDB Server, and therefore this fork, is licensed under version 2 of the
GNU General Public License (GPLv2), without the "any later version" clause.
See [COPYING](COPYING) and [THIRDPARTY](THIRDPARTY).
