// prepublishOnly gate: the WASM artifacts are gitignored, so they must be
// built (./build.sh) before publishing.
import { accessSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dist = join(dirname(fileURLToPath(import.meta.url)), '..', 'dist');
const required = [
  'lite4mariadb.wasm',
  'lite4mariadb.js',
  'index.mjs',
  'index.d.ts',
  'worker.mjs',
  'worker-entry.mjs',
  'worker.d.ts',
];

const missing = required.filter((f) => {
  try {
    accessSync(join(dist, f));
    return false;
  } catch {
    return true;
  }
});

if (missing.length) {
  console.error(
    `dist/ is incomplete (missing: ${missing.join(', ')}).\n` +
      'Run ./build.sh first to compile the WASM module.'
  );
  process.exit(1);
}
