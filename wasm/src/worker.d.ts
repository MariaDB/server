/** Options for Lite4MariaDBWorker.create() — same as the main-package options. */
export interface Lite4MariaDBWorkerOptions {
  dataDir?: string;
  loadDataDir?: Uint8Array | ArrayBuffer | Blob;
  [key: string]: unknown;
}

export type SqlParam =
  | null
  | number
  | bigint
  | boolean
  | string
  | Date
  | Uint8Array
  | ArrayBuffer
  | SqlParam[];

export interface ExecResult {
  ok: true;
  affected?: number;
  fields?: string[];
  types?: [number, number, number][];
  rows: Record<string, unknown>[];
}

export interface WorkerTransaction {
  query<T = Record<string, unknown>>(sql: string, params?: SqlParam[]): Promise<T[]>;
  exec(sql: string, params?: SqlParam[]): Promise<ExecResult>;
  execMulti(sql: string): Promise<ExecResult[]>;
}

/**
 * A Lite4MariaDB instance hosted in a worker (browser DedicatedWorker or Node
 * worker_threads). Same API as Lite4MariaDB, but every method is async.
 */
export class Lite4MariaDBWorker {
  private constructor(worker: unknown);

  /**
   * Spawn (or attach to) a worker running dist/worker-entry.mjs and open the
   * database inside it. Pass your own Worker as the second argument to
   * customize bundling or locateFile.
   */
  static create(
    options?: Lite4MariaDBWorkerOptions,
    worker?: unknown
  ): Promise<Lite4MariaDBWorker>;

  query<T = Record<string, unknown>>(sql: string, params?: SqlParam[]): Promise<T[]>;
  exec(sql: string, params?: SqlParam[]): Promise<ExecResult>;
  execMulti(sql: string): Promise<ExecResult[]>;
  transaction<T>(cb: (tx: WorkerTransaction) => Promise<T>): Promise<T>;
  persist(): Promise<void>;
  dumpDataDir(options?: { compress?: boolean }): Promise<Uint8Array>;
  /** Close the database and terminate the worker. */
  close(): Promise<void>;
}

export default Lite4MariaDBWorker;
