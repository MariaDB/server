# Running The Refactored Parquet Engine

This guide is for the current Parquet engine after the config refactor:

- no `.env`
- no `PARQUET_*` environment variables
- startup config comes from Parquet plugin/system variables
- per-table non-secret routing comes from `CATALOG='...'` and
  `CONNECTION='...'`

These instructions are path-neutral for a macOS checkout. Start from the repo
root and define a few reusable variables:

```bash
cd /path/to/your/server/checkout

export REPO_ROOT="$(pwd)"
export BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
export CNF_PATH="${CNF_PATH:-$BUILD_DIR/mariadb-parquet.cnf}"
export SOCKET_PATH="${SOCKET_PATH:-/tmp/mariadb-parquet.sock}"
export TMP_DIR="${TMP_DIR:-$BUILD_DIR/parquet-tmp}"
```

If your build directory is not `build/`, set `BUILD_DIR` first and the rest of
the commands below will still work.

## 1. Build the new code

From the repo root:

```bash
cd "$REPO_ROOT"

cmake --build "$BUILD_DIR" --target parquet --parallel 4
cmake --build "$BUILD_DIR" --target mariadbd --parallel 4
```

If you changed Parquet code again, rerun those two commands before starting the
server.

## 2. Put Parquet startup config in `mariadb-parquet.cnf`

Open:

`$CNF_PATH`

Add or update the `[mariadbd]` section with Parquet plugin vars:

```ini
[mariadbd]
parquet_lakekeeper_base_url=http://localhost:8181/catalog/v1/
parquet_lakekeeper_warehouse_id=REPLACE_WITH_WAREHOUSE_UUID
parquet_lakekeeper_namespace=data
parquet_lakekeeper_bearer_token=REPLACE_IF_YOU_USE_AUTH
parquet_s3_bucket=mariadb-parquet-demo
parquet_s3_data_prefix=data
parquet_s3_region=us-east-2
parquet_s3_access_key_id=REPLACE_ME
parquet_s3_secret_access_key=REPLACE_ME
```

Important:

- `parquet_lakekeeper_base_url` should include `/catalog/v1/`
- secrets belong here, not in table DDL
- these variables are startup/read-only, so restart `mariadbd` after changes
- `SHOW VARIABLES LIKE 'parquet_%'` will mask secrets as `*****`

If you also need a standalone local socket for this run, make sure the config
either already sets a socket path or add:

```ini
[mariadbd]
socket=/tmp/mariadb-parquet.sock
```

If you use a different socket path, update `SOCKET_PATH` in your shell before
running the client commands below.

## 3. Start LakeKeeper

If LakeKeeper is not already running:

```bash
cd "$REPO_ROOT/lakekeeper/examples/minimal"
docker compose up -d
curl http://localhost:8181/health
```

You want a healthy response before you start MariaDB.

## 4. Create the warehouse and namespace

Create the warehouse once if you do not already have one:

```bash
curl -X POST http://localhost:8181/management/v1/warehouse \
  -H "Content-Type: application/json" \
  -d '{
    "warehouse-name": "default",
    "storage-profile": {
      "type": "s3",
      "bucket": "mariadb-parquet-demo",
      "region": "us-east-2",
      "flavor": "aws",
      "sts-enabled": false
    },
    "storage-credential": {
      "type": "s3",
      "credential-type": "access-key",
      "aws-access-key-id": "REPLACE_ME",
      "aws-secret-access-key": "REPLACE_ME"
    }
  }'
```

Copy the returned warehouse UUID into:

- `parquet_lakekeeper_warehouse_id`

If your config uses `parquet_lakekeeper_namespace=data`, create it once:

```bash
curl -X POST \
  http://localhost:8181/catalog/v1/REPLACE_WITH_WAREHOUSE_UUID/namespaces \
  -H "Content-Type: application/json" \
  -d '{"namespace":["data"]}'
```

## 5. Initialize the datadir if needed

If this local datadir already exists and is valid, you can skip this.

```bash
mkdir -p "$TMP_DIR"

"$BUILD_DIR/scripts/mariadb-install-db" \
  --force \
  --auth-root-authentication-method=socket \
  --auth-root-socket-user="$(whoami)" \
  --srcdir="$REPO_ROOT" \
  --builddir="$BUILD_DIR" \
  --defaults-file="$CNF_PATH"
```

## 6. Start MariaDB with the new Parquet code

```bash
"$BUILD_DIR/sql/mariadbd" \
  --defaults-file="$CNF_PATH"
```

If another local MariaDB is already using the same datadir or socket, stop it
first or point this config at a different datadir/socket.

## 7. Connect

```bash
"$BUILD_DIR/client/mariadb" \
  --defaults-file="$CNF_PATH" \
  --protocol=SOCKET \
  --socket="$SOCKET_PATH"
```

## 8. Verify the Parquet startup variables

Run:

```sql
SHOW VARIABLES LIKE 'parquet_%';
SHOW VARIABLES LIKE 'parquet%token';
SHOW VARIABLES LIKE 'parquet%s3%key%';
```

You should see the new `parquet_*` plugin vars, and the secret values should
appear as `*****`.

## 9. Create a Parquet table with the new config model

Use plugin vars for secrets, and keep table DDL non-secret.

Example:

```sql
CREATE DATABASE IF NOT EXISTS test;
USE test;

DROP TABLE IF EXISTS t1;

CREATE TABLE t1 (
  id BIGINT,
  name VARCHAR(50)
) ENGINE=PARQUET
  CATALOG='table=t1_iceberg;namespace=data'
  CONNECTION='bucket=mariadb-test-parquet-2;key_prefix=data';
```

That example relies on startup vars for:

- LakeKeeper base URL
- warehouse ID
- object-store region
- access key
- secret key

You can override more non-secret routing per table:

```sql
CREATE TABLE t2 (
  id BIGINT,
  name VARCHAR(50)
) ENGINE=PARQUET
  CATALOG='uri=http://localhost:8181/catalog/v1/;warehouse=REPLACE_WITH_WAREHOUSE_UUID;namespace=data;table=t2_iceberg'
  CONNECTION='endpoint=https://s3.us-east-2.amazonaws.com;bucket=mariadb-parquet-demo;region=us-east-2;key_prefix=data';
```

## 10. Confirm the table metadata behavior

After creating a table, you should have:

- a Parquet data file under your datadir, usually something like
  `$BUILD_DIR/parquet-data/test/t1.parquet`
- a metadata sidecar next to it, usually something like
  `$BUILD_DIR/parquet-data/test/t1.parquet.meta`

The exact datadir path depends on your MariaDB config. The `.parquet.meta`
sidecar should contain resolved non-secret routing, but it should not contain:

- bearer tokens
- access keys
- secret keys
- session tokens

## 11. Quick smoke queries

```sql
INSERT INTO t1 VALUES (1, 'alpha'), (2, 'beta');
SELECT * FROM t1 ORDER BY id;
SHOW CREATE TABLE t1;
```

`SHOW CREATE TABLE` should preserve the explicit `CATALOG`/`CONNECTION` options
you wrote in the DDL, but it should not show plugin-defaulted secrets.

## 12. Useful local test commands

Parquet-only build:

```bash
cmake --build "$BUILD_DIR" --target parquet --parallel 4
```

Parquet mysql-test smoke:

```bash
perl "$BUILD_DIR/mysql-test/mysql-test-run.pl" \
  --suite=parquet linux_engine_smoke
```

Parquet plugin-var coverage:

```bash
perl "$BUILD_DIR/mysql-test/mysql-test-run.pl" \
  --suite=parquet plugin_vars
```

If `mysql-test-run.pl` complains about socket path length on macOS, rerun it
from a shorter absolute checkout path or shorten the temporary test layout.

## 13. Troubleshooting

If `SHOW VARIABLES LIKE 'parquet_%'` comes back empty:

- make sure you rebuilt `mariadbd`, not just the Parquet target
- fully restart the server after rebuilding
- confirm your client is connecting to the same socket path that the rebuilt
  server is actually using

If secrets show up in cleartext instead of `*****`:

- you are probably connected to an older `mariadbd` binary
- restart the server from `"$BUILD_DIR/sql/mariadbd"` and reconnect

If a second local MariaDB instance is already running:

- use a different socket path in your config and in `SOCKET_PATH`
- or shut down the conflicting local server before starting this one
