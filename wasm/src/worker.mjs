// Main-thread client for a MariaDBlite instance hosted in a worker.
// Browser:
//   const db = await MariaDBliteWorker.create({ dataDir: 'idb://app' });
// Node (worker_threads) works the same way.
// To customize (own bundler entry, locateFile, ...), pass your own Worker
// running dist/worker-entry.mjs as the second argument.

async function spawnDefaultWorker() {
  const url = new URL('./worker-entry.mjs', import.meta.url);
  if (typeof process !== 'undefined' && process.versions?.node) {
    const { Worker } = await import('node:worker_threads');
    return new Worker(url);
  }
  return new Worker(url, { type: 'module' });
}

export class MariaDBliteWorker {
  constructor(worker) {
    this._worker = worker;
    this._seq = 0;
    this._pending = new Map();
    const onMsg = (data) => {
      const { id, ok, result, error } = data;
      const p = this._pending.get(id);
      if (!p) return;
      this._pending.delete(id);
      if (ok) {
        p.resolve(result);
      } else {
        const err = new Error(error?.message || 'worker error');
        if (error?.errno !== undefined && error.errno !== null) {
          err.errno = error.errno;
        }
        if (error?.sqlstate) err.sqlstate = error.sqlstate;
        p.reject(err);
      }
    };
    if (worker.addEventListener) {
      worker.addEventListener('message', (e) => onMsg(e.data));
      worker.addEventListener('error', (e) =>
        this._rejectAll(new Error(e.message || 'worker error'))
      );
    } else {
      worker.on('message', onMsg);
      worker.on('error', (err) => this._rejectAll(err));
    }
  }

  static async create(options = {}, worker) {
    const w = new MariaDBliteWorker(worker ?? (await spawnDefaultWorker()));
    await w._call('create', [options]);
    return w;
  }

  _rejectAll(err) {
    for (const p of this._pending.values()) p.reject(err);
    this._pending.clear();
  }

  _call(op, args) {
    const id = ++this._seq;
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      this._worker.postMessage({ id, op, args });
    });
  }

  query(sql, params) {
    return this._call('query', [sql, params]);
  }

  exec(sql, params) {
    return this._call('exec', [sql, params]);
  }

  execMulti(sql) {
    return this._call('execMulti', [sql]);
  }

  async transaction(cb) {
    await this.exec('START TRANSACTION');
    const tx = {
      query: (sql, params) => this.query(sql, params),
      exec: (sql, params) => this.exec(sql, params),
      execMulti: (sql) => this.execMulti(sql),
    };
    try {
      const result = await cb(tx);
      await this.exec('COMMIT');
      return result;
    } catch (err) {
      await this.exec('ROLLBACK');
      throw err;
    }
  }

  persist() {
    return this._call('persist');
  }

  dumpDataDir(options) {
    return this._call('dumpDataDir', [options]);
  }

  async close() {
    await this._call('close');
    if (this._worker.terminate) await this._worker.terminate();
  }
}

export default MariaDBliteWorker;
