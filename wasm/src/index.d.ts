/** Options for Lite4MariaDB.create(). */
export interface Lite4MariaDBOptions {
  /**
   * Where to store the database:
   * - `memory://` (default) — ephemeral in-memory
   * - `./path`, `file://…`, `node://…` — real directory (Node.js)
   * - `idb://name` — IndexedDB (browser)
   */
  dataDir?: string;
  /** Tarball produced by dumpDataDir() to load into a fresh, empty datadir. */
  loadDataDir?: Uint8Array | ArrayBuffer | Blob;
  /** Custom resolver for the .wasm asset. */
  locateFile?: (path: string, prefix: string) => string;
  /** Additional Emscripten module options. */
  [key: string]: unknown;
}

/** Values bindable to `?` placeholders. Arrays expand for `IN ?`. */
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
  /** Rows affected; present for non-SELECT statements. */
  affected?: number;
  /** Column names; present for result-set statements. */
  fields?: string[];
  /** Per-column [type, charsetnr, flags] metadata. */
  types?: [number, number, number][];
  rows: Record<string, unknown>[];
}

/** Transaction facade passed to the transaction() callback. */
export interface Transaction {
  query<T = Record<string, unknown>>(sql: string, params?: SqlParam[]): T[];
  exec(sql: string, params?: SqlParam[]): ExecResult;
  execMulti(sql: string): ExecResult[];
}

export class Lite4MariaDB {
  private constructor(mod: unknown, fsSpec: unknown);

  static create(options?: Lite4MariaDBOptions): Promise<Lite4MariaDB>;

  /** Which storage backend is in use. */
  readonly fsType: 'memory' | 'node' | 'idb';
  /** Called when a debounced background persist (idb://) fails. */
  onPersistError?: (err: Error) => void;

  /** Run a statement, return its rows with coerced JS types. */
  query<T = Record<string, unknown>>(sql: string, params?: SqlParam[]): T[];
  /** Run a statement, return the full result including affected rows. */
  exec(sql: string, params?: SqlParam[]): ExecResult;
  /** Run a multi-statement script; one result per statement. */
  execMulti(sql: string): ExecResult[];
  /** BEGIN/COMMIT around the callback, ROLLBACK if it throws. */
  transaction<T>(cb: (tx: Transaction) => Promise<T> | T): Promise<T>;
  /** Flush the in-memory view to IndexedDB (idb:// only). */
  persist(): Promise<void>;
  /** Snapshot the datadir as a (gzipped) tar Uint8Array. */
  dumpDataDir(options?: { compress?: boolean }): Promise<Uint8Array>;
  /** Shut down the server; flushes idb:// first. */
  close(): Promise<void>;
}

export default Lite4MariaDB;
