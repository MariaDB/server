import createMariaDBlite from './mariadblite.js';

const DATA_DIR = '/mariadb/data';
const IDB_ROOT = '/mariadb/idb';

function wasmUrl(path) {
  return new URL(path, import.meta.url).href;
}

// PGlite-style dataDir: memory:// (default), bare path or file:// (Node,
// real on-disk directory via NODEFS), idb://name (browser, IndexedDB).
function parseDataDir(dataDir) {
  if (!dataDir || dataDir === 'memory://') return { type: 'memory' };
  if (dataDir.startsWith('idb://')) {
    const name = dataDir.slice('idb://'.length).replace(/^\/+|\/+$/g, '');
    if (!name) throw new Error('mariadblite: idb:// dataDir needs a name');
    return { type: 'idb', name };
  }
  for (const scheme of ['file://', 'node://']) {
    if (dataDir.startsWith(scheme)) {
      return { type: 'node', path: dataDir.slice(scheme.length) };
    }
  }
  return { type: 'node', path: dataDir };
}

function syncfs(mod, populate) {
  return new Promise((resolve, reject) => {
    mod.FS.syncfs(populate, (err) => (err ? reject(err) : resolve()));
  });
}

function parseEnvelope(mod, ptr) {
  if (!ptr) {
    throw new Error('mariadblite: empty result pointer');
  }
  const json = mod.UTF8ToString(ptr);
  mod._mdl_free(ptr);
  let res;
  try {
    res = JSON.parse(json);
  } catch (e) {
    throw new Error(`mariadblite: invalid JSON from engine: ${json}`);
  }
  if (!res.ok) {
    const err = new Error(res.error || 'query failed');
    err.errno = res.errno;
    if (res.sqlstate) err.sqlstate = res.sqlstate;
    throw err;
  }
  return res;
}

// ---------------------------------------------------------------------------
// Type coercion: the wire format is all strings (binary as {$h: hex}); map
// to JS types using the [type, charsetnr, flags] metadata per column.
// ---------------------------------------------------------------------------

const FIELD = {
  DECIMAL: 0,
  TINY: 1,
  SHORT: 2,
  LONG: 3,
  FLOAT: 4,
  DOUBLE: 5,
  TIMESTAMP: 7,
  LONGLONG: 8,
  INT24: 9,
  DATE: 10,
  TIME: 11,
  DATETIME: 12,
  YEAR: 13,
  BIT: 16,
  JSON: 245,
  NEWDECIMAL: 246,
};

function hexToBytes(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16);
  }
  return out;
}

function bytesToHex(bytes) {
  let s = '';
  for (const b of bytes) s += b.toString(16).padStart(2, '0');
  return s;
}

function coerceValue(raw, type) {
  if (raw === null) return null;
  if (typeof raw === 'object' && '$h' in raw) {
    return hexToBytes(raw.$h);
  }
  switch (type) {
    case FIELD.TINY:
    case FIELD.SHORT:
    case FIELD.LONG:
    case FIELD.INT24:
    case FIELD.YEAR:
      return Number(raw);
    case FIELD.LONGLONG: {
      const n = Number(raw);
      return Number.isSafeInteger(n) ? n : raw;
    }
    case FIELD.FLOAT:
    case FIELD.DOUBLE:
      return Number(raw);
    case FIELD.JSON:
      try {
        return JSON.parse(raw);
      } catch {
        return raw;
      }
    default:
      // DECIMAL stays a string (exactness), DATE/TIME/DATETIME/TIMESTAMP stay
      // strings (timezone-safe), text/enum/set pass through.
      return raw;
  }
}

function coerceRows(res) {
  if (!res.types || !res.rows) return;
  for (const row of res.rows) {
    for (let i = 0; i < res.fields.length; i++) {
      row[res.fields[i]] = coerceValue(row[res.fields[i]], res.types[i][0]);
    }
  }
}

// ---------------------------------------------------------------------------
// Parameterized queries: substitute ? placeholders outside string literals
// and comments with properly escaped literals.
// ---------------------------------------------------------------------------

function escapeString(mod, s) {
  const ptr = mod.ccall('mdl_escape', 'number', ['string'], [s]);
  if (!ptr) throw new Error('mariadblite: escape failed');
  const out = mod.UTF8ToString(ptr);
  mod._mdl_free(ptr);
  return out;
}

function toLiteral(mod, v) {
  if (v === null || v === undefined) return 'NULL';
  switch (typeof v) {
    case 'number':
      if (!Number.isFinite(v)) {
        throw new Error('mariadblite: cannot bind NaN or Infinity');
      }
      return String(v);
    case 'bigint':
      return v.toString();
    case 'boolean':
      return v ? '1' : '0';
    case 'string':
      return `'${escapeString(mod, v)}'`;
    case 'object': {
      if (v instanceof Date) {
        return `'${v.toISOString().slice(0, 23).replace('T', ' ')}'`;
      }
      if (Array.isArray(v)) {
        return `(${v.map((x) => toLiteral(mod, x)).join(',')})`;
      }
      if (v instanceof Uint8Array) {
        return `X'${bytesToHex(v)}'`;
      }
      if (v instanceof ArrayBuffer) {
        return `X'${bytesToHex(new Uint8Array(v))}'`;
      }
      if (ArrayBuffer.isView(v)) {
        return `X'${bytesToHex(new Uint8Array(v.buffer, v.byteOffset, v.byteLength))}'`;
      }
      throw new Error('mariadblite: unsupported parameter type');
    }
    default:
      throw new Error(`mariadblite: unsupported parameter type ${typeof v}`);
  }
}

function substituteParams(mod, sql, params) {
  if (!params || params.length === 0) return sql;
  let out = '';
  let i = 0;
  let p = 0;
  const n = sql.length;
  while (i < n) {
    const c = sql[i];
    if (
      (c === '-' && sql[i + 1] === '-' && /\s|^/.test(sql[i + 2] || '')) ||
      c === '#'
    ) {
      const end = sql.indexOf('\n', i);
      const stop = end === -1 ? n : end;
      out += sql.slice(i, stop);
      i = stop;
      continue;
    }
    if (c === '/' && sql[i + 1] === '*') {
      const end = sql.indexOf('*/', i + 2);
      const stop = end === -1 ? n : end + 2;
      out += sql.slice(i, stop);
      i = stop;
      continue;
    }
    if (c === "'" || c === '"' || c === '`') {
      const quote = c;
      let j = i + 1;
      while (j < n) {
        if (sql[j] === '\\' && quote !== '`') {
          j += 2;
          continue;
        }
        if (sql[j] === quote) {
          if (quote !== '`' && sql[j + 1] === quote) {
            j += 2;
            continue;
          }
          break;
        }
        j++;
      }
      out += sql.slice(i, j + 1);
      i = j + 1;
      continue;
    }
    if (c === '?') {
      if (p >= params.length) {
        throw new Error('mariadblite: more placeholders than parameters');
      }
      out += toLiteral(mod, params[p++]);
      i++;
      continue;
    }
    out += c;
    i++;
  }
  if (p < params.length) {
    throw new Error('mariadblite: more parameters than placeholders');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Tar snapshots (dumpDataDir / loadDataDir), USTAR format.
// ---------------------------------------------------------------------------

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

function tarHeader(name, size, typeflag, mtime) {
  const buf = new Uint8Array(512);
  const writeStr = (s, off, len) => {
    buf.set(textEncoder.encode(s).subarray(0, len - 1), off);
  };
  const writeOctal = (v, off, len) => {
    writeStr(v.toString(8).padStart(len - 1, '0'), off, len);
  };
  let prefix = '';
  if (textEncoder.encode(name).length > 99) {
    // USTAR split: name <= 100, prefix <= 155, split at a slash.
    const parts = name.split('/');
    let idx = parts.length - 1;
    let tail = parts[idx];
    while (idx > 0 && tail.length + 1 + parts[idx - 1].length <= 99) {
      idx--;
      tail = parts[idx] + '/' + tail;
    }
    prefix = parts.slice(0, idx).join('/');
    name = tail;
    if (name.length > 99 || prefix.length > 155) {
      throw new Error(`mariadblite: tar path too long: ${parts.join('/')}`);
    }
  }
  writeStr(name, 0, 100);
  writeOctal(0o644, 100, 8);
  writeOctal(0, 108, 8);
  writeOctal(0, 116, 8);
  writeOctal(size, 124, 12);
  writeOctal(Math.floor(mtime / 1000), 136, 12);
  buf.fill(0x20, 148, 156);
  buf[156] = textEncoder.encode(typeflag)[0];
  writeStr('ustar', 257, 6);
  buf[263] = 0x30;
  buf[264] = 0x30;
  if (prefix) writeStr(prefix, 345, 155);
  let sum = 0;
  for (let i = 0; i < 512; i++) sum += buf[i];
  writeStr(sum.toString(8).padStart(6, '0'), 148, 8);
  return buf;
}

function walkFs(FS, root) {
  const out = [];
  const walk = (abs, rel) => {
    for (const entry of FS.readdir(abs)) {
      if (entry === '.' || entry === '..') continue;
      const childAbs = `${abs}/${entry}`;
      const childRel = rel ? `${rel}/${entry}` : entry;
      const stat = FS.stat(childAbs);
      if (FS.isDir(stat.mode)) {
        out.push({ path: childRel, type: 'dir', mtime: stat.mtime });
        walk(childAbs, childRel);
      } else {
        out.push({
          path: childRel,
          type: 'file',
          data: FS.readFile(childAbs),
          mtime: stat.mtime,
        });
      }
    }
  };
  walk(root, '');
  return out;
}

function concatBytes(chunks) {
  let total = 0;
  for (const c of chunks) total += c.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

function dumpTar(FS, root) {
  const chunks = [];
  for (const e of walkFs(FS, root)) {
    const size = e.type === 'file' ? e.data.length : 0;
    chunks.push(tarHeader(e.path, size, e.type === 'dir' ? '5' : '0', e.mtime));
    if (e.type === 'file') {
      chunks.push(e.data);
      const pad = (512 - (e.data.length % 512)) % 512;
      if (pad) chunks.push(new Uint8Array(pad));
    }
  }
  chunks.push(new Uint8Array(1024));
  return concatBytes(chunks);
}

function tarReadStr(header, off, len) {
  let end = off;
  const max = off + len;
  while (end < max && header[end] !== 0) end++;
  return textDecoder.decode(header.subarray(off, end));
}

function untarInto(FS, data, dest) {
  let off = 0;
  let longName = null;
  while (off + 512 <= data.length) {
    const header = data.subarray(off, off + 512);
    off += 512;
    if (header[0] === 0) break;
    let name = tarReadStr(header, 0, 100);
    const prefix = tarReadStr(header, 345, 155);
    if (prefix) name = `${prefix}/${name}`;
    const size = parseInt(tarReadStr(header, 124, 12).trim() || '0', 8);
    const typeflag = String.fromCharCode(header[156]);
    if (typeflag === 'L') {
      longName = textDecoder
        .decode(data.subarray(off, off + size))
        .replace(/\0[\s\S]*$/, '');
      off += Math.ceil(size / 512) * 512;
      continue;
    }
    if (longName) {
      name = longName;
      longName = null;
    }
    const target = `${dest}/${name}`;
    if (typeflag === '5') {
      FS.mkdirTree(target);
    } else if (typeflag === '0' || typeflag === '\0') {
      const slash = target.lastIndexOf('/');
      if (slash > 0) FS.mkdirTree(target.slice(0, slash));
      FS.writeFile(target, data.subarray(off, off + size));
    }
    off += Math.ceil(size / 512) * 512;
  }
}

async function gzipBytes(data) {
  const stream = new Blob([data]).stream().pipeThrough(new CompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function gunzipBytes(data) {
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function toBytes(payload) {
  if (payload instanceof Uint8Array) return payload;
  if (payload instanceof ArrayBuffer) return new Uint8Array(payload);
  if (ArrayBuffer.isView(payload)) {
    return new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
  }
  if (payload && typeof payload.arrayBuffer === 'function') {
    return new Uint8Array(await payload.arrayBuffer());
  }
  throw new Error('mariadblite: loadDataDir expects Uint8Array/ArrayBuffer/Blob');
}

// ---------------------------------------------------------------------------

export class MariaDBlite {
  constructor(mod, fsSpec) {
    this.mod = mod;
    this.fsType = fsSpec.type;
    this._fsSpec = fsSpec;
    this._persistTimer = null;
    this._persisting = null;
  }

  static async create(options = {}) {
    const { dataDir, loadDataDir, locateFile: userLocate, ...rest } = options;
    const fsSpec = parseDataDir(dataDir);
    const mod = await createMariaDBlite({
      locateFile: (p, prefix) =>
        userLocate ? userLocate(p, prefix) : wasmUrl(p),
      ...rest,
    });
    const db = new MariaDBlite(mod, fsSpec);
    await db._mount();
    if (loadDataDir) {
      await db._loadDataDir(loadDataDir);
    }
    const rc = mod._mdl_open();
    if (rc !== 0) {
      throw new Error(`mariadblite: mdl_open failed (${rc})`);
    }
    return db;
  }

  async _mount() {
    const mod = this.mod;
    if (this.fsType === 'memory') return;

    if (this.fsType === 'node') {
      if (typeof process === 'undefined' || !process.versions?.node) {
        throw new Error('mariadblite: file/node dataDir requires Node.js');
      }
      if (!mod.FS.filesystems.NODEFS) {
        throw new Error('mariadblite: NODEFS not linked into this build');
      }
      const { mkdirSync } = await import('node:fs');
      const { resolve } = await import('node:path');
      const root = resolve(this._fsSpec.path);
      mkdirSync(root, { recursive: true });
      mod.FS.mkdirTree(DATA_DIR);
      mod.FS.mount(mod.FS.filesystems.NODEFS, { root }, DATA_DIR);
      return;
    }

    // idb:// — like PGlite's IdbFs: mount IDBFS under a per-name path so each
    // database gets its own IndexedDB store, then symlink the fixed datadir.
    if (typeof indexedDB === 'undefined') {
      throw new Error('mariadblite: idb:// requires a browser with IndexedDB');
    }
    if (!mod.FS.filesystems.IDBFS) {
      throw new Error('mariadblite: IDBFS not linked into this build');
    }
    const mountPoint = `${IDB_ROOT}/${this._fsSpec.name}`;
    mod.FS.mkdirTree(mountPoint);
    mod.FS.mount(mod.FS.filesystems.IDBFS, {}, mountPoint);
    mod.FS.symlink(mountPoint, DATA_DIR);
    await syncfs(mod, true);
  }

  async _loadDataDir(payload) {
    const mod = this.mod;
    let bytes = await toBytes(payload);
    if (bytes[0] === 0x1f && bytes[1] === 0x8b) {
      bytes = await gunzipBytes(bytes);
    }
    mod.FS.mkdirTree(DATA_DIR);
    const existing = mod.FS.readdir(DATA_DIR).filter(
      (e) => e !== '.' && e !== '..'
    );
    if (existing.length) {
      throw new Error('mariadblite: datadir not empty, cannot loadDataDir');
    }
    untarInto(mod.FS, bytes, DATA_DIR);
  }

  query(sql, params) {
    const final = substituteParams(this.mod, String(sql), params);
    const ptr = this.mod.ccall('mdl_query', 'number', ['string'], [final]);
    const res = parseEnvelope(this.mod, ptr);
    coerceRows(res);
    this._schedulePersist();
    return res.rows;
  }

  exec(sql, params) {
    const final = substituteParams(this.mod, String(sql), params);
    const ptr = this.mod.ccall('mdl_query', 'number', ['string'], [final]);
    const res = parseEnvelope(this.mod, ptr);
    coerceRows(res);
    this._schedulePersist();
    return res;
  }

  // Multi-statement script; returns one result object per statement.
  execMulti(sql) {
    const ptr = this.mod.ccall('mdl_exec_multi', 'number', ['string'], [String(sql)]);
    const res = parseEnvelope(this.mod, ptr);
    for (const r of res.results) coerceRows(r);
    this._schedulePersist();
    return res.results;
  }

  async transaction(cb) {
    this.exec('START TRANSACTION');
    const tx = {
      query: (sql, params) => this.query(sql, params),
      exec: (sql, params) => this.exec(sql, params),
      execMulti: (sql) => this.execMulti(sql),
    };
    try {
      const result = await cb(tx);
      this.exec('COMMIT');
      return result;
    } catch (err) {
      this.exec('ROLLBACK');
      throw err;
    }
  }

  // Gzipped tar of the datadir (Uint8Array), like PGlite's dumpDataDir.
  // InnoDB redo makes a live dump crash-recoverable; close() first for a
  // clean snapshot.
  async dumpDataDir({ compress = true } = {}) {
    if (!this.mod) throw new Error('mariadblite: database is closed');
    const tar = dumpTar(this.mod.FS, DATA_DIR);
    return compress ? gzipBytes(tar) : tar;
  }

  // IDBFS is an in-memory overlay: schedule a flush after each statement,
  // debounced, so a burst of queries persists once.
  _schedulePersist() {
    if (this.fsType !== 'idb' || !this.mod) return;
    if (this._persistTimer) clearTimeout(this._persistTimer);
    this._persistTimer = setTimeout(() => {
      this._persistTimer = null;
      this.persist().catch((err) => {
        (this.onPersistError || console.error)(err);
      });
    }, 100);
    if (this._persistTimer.unref) this._persistTimer.unref();
  }

  async persist() {
    if (this.fsType !== 'idb' || !this.mod) return;
    if (this._persistTimer) {
      clearTimeout(this._persistTimer);
      this._persistTimer = null;
    }
    if (this._persisting) return this._persisting;
    this._persisting = syncfs(this.mod, false).finally(() => {
      this._persisting = null;
    });
    return this._persisting;
  }

  async close() {
    if (!this.mod) return;
    const mod = this.mod;
    this.mod = null;
    if (this._persistTimer) {
      clearTimeout(this._persistTimer);
      this._persistTimer = null;
    }
    if (this._persisting) await this._persisting;
    mod._mdl_close();
    // InnoDB shutdown wrote everything to the in-memory view; flush last.
    if (this.fsType === 'idb') await syncfs(mod, false);
  }
}

export default MariaDBlite;
