// Default worker entry: serves one MariaDBlite instance over postMessage RPC.
// Works in a browser DedicatedWorker and in a Node worker_thread.
import { MariaDBlite } from './index.mjs';

const isNode =
  typeof process !== 'undefined' && !!process.versions?.node;

let port;
if (isNode) {
  ({ parentPort: port } = await import('node:worker_threads'));
} else {
  port = globalThis.self;
}

let db = null;

const handlers = {
  async create([options]) {
    if (db) throw new Error('mariadblite worker: already open');
    db = await MariaDBlite.create(options ?? {});
    return true;
  },
  query([sql, params]) {
    return db.query(sql, params);
  },
  exec([sql, params]) {
    return db.exec(sql, params);
  },
  execMulti([sql]) {
    return db.execMulti(sql);
  },
  persist() {
    return db.persist();
  },
  dumpDataDir([options]) {
    return db.dumpDataDir(options);
  },
  async close() {
    await db.close();
    db = null;
    return true;
  },
};

port.onmessage = async (e) => {
  const { id, op, args } = e.data;
  try {
    if (!handlers[op]) throw new Error(`mariadblite worker: unknown op ${op}`);
    const result = await handlers[op](args ?? []);
    port.postMessage({ id, ok: true, result });
  } catch (err) {
    port.postMessage({
      id,
      ok: false,
      error: {
        message: err?.message ?? String(err),
        errno: err?.errno,
        sqlstate: err?.sqlstate,
      },
    });
  }
};
