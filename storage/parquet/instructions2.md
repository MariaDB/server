# Local Setup Instructions (`/Users/ryanzhou/Desktop/server`)

These instructions match a working local setup for this checkout and are written for the current tree layout under:

`/Users/ryanzhou/Desktop/server`

Use the exact absolute paths below unless your checkout lives somewhere else.

## 0. Paths and current working values

Important local paths:

- source tree: `/Users/ryanzhou/Desktop/server`
- build tree: `/Users/ryanzhou/Desktop/server/build`
- runtime env file: `/Users/ryanzhou/Desktop/server/.env`
- MariaDB config: `/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf`
- MariaDB socket: `/tmp/mariadb-parquet.sock`
- LakeKeeper checkout: `/Users/ryanzhou/Desktop/server/lakekeeper`

Current non-secret runtime values in this checkout:
THE LAKEKEEPER DEFAULT VALUES WILL NOT WORK IF YOU ARE HAVE NOT INITIALIZED LAKEKEEPER YET. REPLACE WITH YOUR OWN.

- `PARQUET_LAKEKEEPER_BASE_URL=http://localhost:8181/catalog/v1/`
- `PARQUET_LAKEKEEPER_WAREHOUSE_ID=34222c1a-3c39-11f1-8407-8f978f046b38`
- `PARQUET_LAKEKEEPER_NAMESPACE=data`
- `PARQUET_S3_BUCKET=mariadb-parquet-demo`
- `PARQUET_S3_DATA_PREFIX=data`
- `PARQUET_S3_REGION=us-east-2`

Important notes:

- `PARQUET_LAKEKEEPER_BASE_URL` must include the `/catalog/v1/` suffix.
- `PARQUET_S3_DATA_PREFIX=data` means Parquet objects are written under `s3://mariadb-parquet-demo/data/...`, not at the bucket root.
- The Parquet engine reads LakeKeeper and S3 settings from `.env` when `mariadbd` starts, so restart the server after editing `.env`.
- Do not commit real AWS access keys from `.env`.
- LakeKeeper must be reachable before running `mariadb-install-db` for this setup, because Parquet code can touch LakeKeeper during bootstrap.

## 1. Configure and build the server

Run these from the repo root:

```bash
cd /Users/ryanzhou/Desktop/server

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target minbuild --parallel 4
cmake --build build --target mariadbd --parallel 4
```

Why both targets matter:

- `mariadbd` builds the server binary you will run.
- `minbuild` builds helper tools needed by out-of-tree bootstrap, such as `build/extra/my_print_defaults` and related scripts.

After this step, you should have at least:

- `/Users/ryanzhou/Desktop/server/build/sql/mariadbd`
- `/Users/ryanzhou/Desktop/server/build/scripts/mariadb-install-db`
- `/Users/ryanzhou/Desktop/server/build/build.ninja`

## 2. Prepare `.env`

Update:

`/Users/ryanzhou/Desktop/server/.env`

Use this shape:

```dotenv
PARQUET_LAKEKEEPER_BASE_URL=http://localhost:8181/catalog/v1/
PARQUET_LAKEKEEPER_WAREHOUSE_ID=34222c1a-3c39-11f1-8407-8f978f046b38
PARQUET_LAKEKEEPER_NAMESPACE=data
PARQUET_S3_BUCKET=mariadb-parquet-demo
PARQUET_S3_DATA_PREFIX=data
PARQUET_S3_REGION=us-east-2
PARQUET_S3_ACCESS_KEY_ID=REPLACE_ME
PARQUET_S3_SECRET_ACCESS_KEY=REPLACE_ME
```

If you want to keep the env file somewhere else, set:

```bash
export PARQUET_ENV_FILE=/absolute/path/to/your/.env
```

Restart `mariadbd` after changing `.env`.

## 3. Start LakeKeeper

Use the checked-out LakeKeeper repo in this tree:

```bash
cd /Users/ryanzhou/Desktop/server/lakekeeper/examples/minimal
docker compose up -d
```

Verify it:

```bash
curl http://localhost:8181/health
```

You want a healthy response here before running MariaDB bootstrap.

## 4. Create or verify the warehouse and namespace

If you already created the warehouse with ID `34222c1a-3c39-11f1-8407-8f978f046b38`, you can keep using it. If you need a fresh warehouse, create one with your real AWS credentials:

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

- `PARQUET_LAKEKEEPER_WAREHOUSE_ID`

The current `.env` uses namespace `data`, so create that namespace once:

```bash
curl -X POST \
  http://localhost:8181/catalog/v1/34222c1a-3c39-11f1-8407-8f978f046b38/namespaces \
  -H "Content-Type: application/json" \
  -d '{"namespace":["data"]}'
```

If you change the namespace in `.env`, create that namespace instead.

## 5. Initialize or reinitialize the MariaDB data directory

Before initializing, stop any old server bound to the test socket:

```bash
kill $(lsof -t /tmp/mariadb-parquet.sock)
```

Create the temp directory used by the local config:

```bash
mkdir -p /Users/ryanzhou/Desktop/server/build/parquet-tmp
```

If you want a fresh local datadir, remove the old one first:

```bash
rm -rf /Users/ryanzhou/Desktop/server/build/parquet-data
```

Then run the out-of-tree bootstrap command from the repo root:

```bash
cd /Users/ryanzhou/Desktop/server

build/scripts/mariadb-install-db \
  --force \
  --auth-root-authentication-method=socket \
  --auth-root-socket-user="$(whoami)" \
  --srcdir=/Users/ryanzhou/Desktop/server \
  --builddir=/Users/ryanzhou/Desktop/server/build \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

Why this exact command matters:

- Use `build/scripts/mariadb-install-db`, not `scripts/mariadb-install-db` from the source tree.
- Pass `--builddir=/Users/ryanzhou/Desktop/server/build` for an out-of-tree build.
- `--auth-root-authentication-method=socket` keeps local socket auth enabled.
- `--auth-root-socket-user="$(whoami)"` makes the current macOS user able to connect locally without adding `-u root`.

If the bootstrap fails while LakeKeeper is down, you can end up with a partially initialized `mysql.global_priv` table and then see `ERROR 1045 (28000)` for both `ryanzhou` and `root`. In that case:

1. Start LakeKeeper and confirm `/health` works.
2. Remove `build/parquet-data`.
3. Run the bootstrap command again.

If you prefer the older passwordless-root style instead of socket auth, replace:

```text
--auth-root-authentication-method=socket
```

with:

```text
--auth-root-authentication-method=normal
```

## 6. Start MariaDB

Run the build binary, not the source-tree path:

```bash
/Users/ryanzhou/Desktop/server/build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

If startup succeeds, the log at `/Users/ryanzhou/Desktop/server/build/mariadb-parquet.err` should eventually show:

```text
ready for connections
```

You can verify the socket owner with:

```bash
ls -l /tmp/mariadb-parquet.sock
```

## 7. Connect to MariaDB

For the socket-auth flow above, connect as your current shell user:

```bash
cd /Users/ryanzhou/Desktop/server

build/client/mariadb \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf \
  --protocol=SOCKET
```

To connect as `root` with socket auth:

```bash
sudo /Users/ryanzhou/Desktop/server/build/client/mariadb \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf \
  --protocol=SOCKET \
  -u root
```

If you initialized with `--auth-root-authentication-method=normal`, you may instead use the traditional root flow and set a password afterward.

## 8. Quick Parquet smoke test

Use a simple Parquet table first:

```sql
CREATE DATABASE IF NOT EXISTS test;
USE test;

DROP TABLE IF EXISTS t1;

CREATE TABLE t1 (
  id BIGINT,
  name VARCHAR(50)
) ENGINE=PARQUET;

INSERT INTO t1 VALUES (1, 'alpha'), (2, 'beta');
INSERT INTO t1 VALUES (3, 'gamma');

SELECT * FROM t1 ORDER BY id;
SELECT * FROM t1 WHERE id >= 2 ORDER BY id;
```

Expected:

```text
+------+-------+
| id   | name  |
+------+-------+
|    1 | alpha |
|    2 | beta  |
|    3 | gamma |
+------+-------+
```

## 9. Test cross-engine join pushdown

To test the SELECT handler's mixed-engine path, use one Parquet table and one non-Parquet table:

```sql
USE test;

DROP TABLE IF EXISTS p_orders;
DROP TABLE IF EXISTS i_customers;

CREATE TABLE p_orders (
  order_id BIGINT,
  customer_id BIGINT,
  amount BIGINT
) ENGINE=PARQUET;

CREATE TABLE i_customers (
  customer_id BIGINT PRIMARY KEY,
  customer_name VARCHAR(50)
) ENGINE=InnoDB;

INSERT INTO p_orders VALUES
  (1, 10, 100),
  (2, 20, 200),
  (3, 10, 150);

INSERT INTO i_customers VALUES
  (10, 'Alice'),
  (20, 'Bob'),
  (30, 'Carol');

SELECT p.order_id, p.amount, c.customer_name
FROM p_orders AS p
JOIN i_customers AS c
  ON p.customer_id = c.customer_id
ORDER BY p.order_id;

SELECT p.order_id, c.customer_name
FROM p_orders AS p
JOIN i_customers AS c
  ON p.customer_id = c.customer_id
WHERE p.amount >= 150
ORDER BY p.order_id;
```

Expected results:

```text
+----------+--------+---------------+
| order_id | amount | customer_name |
+----------+--------+---------------+
|        1 |    100 | Alice         |
|        2 |    200 | Bob           |
|        3 |    150 | Alice         |
+----------+--------+---------------+

+----------+---------------+
| order_id | customer_name |
+----------+---------------+
|        2 | Bob           |
|        3 | Alice         |
+----------+---------------+
```

After running those queries, inspect the server log:

IMPORTANT (makes sure the program is actually running as intended; the results may look good on the surface but the engine may actually be malfunctioning)

```bash
grep "Parquet:" /Users/ryanzhou/Desktop/server/build/mariadb-parquet.err
```

For the cross-engine path, you should see log lines like:

- `Parquet: selected cross-engine pushdown handler`
- `Parquet: cross-engine pushdown init parquet_tables=[...] external_tables=[...]`
- `Parquet: cross-engine registry add key='i_customers'`
- `Parquet: replacement scan lookup schema='...' table='i_customers'`
- `Parquet: _mdb_scan bind table='i_customers' columns=...`
- `Parquet: _mdb_scan start table='i_customers' projected_columns=...`
- `Parquet: _mdb_scan complete table='i_customers' rows=...`

If the query returns the right result but none of those lines appear, you are probably not running the rebuilt binary from:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```

## 10. Fast rebuild loop after Parquet code changes

If you change files under `storage/parquet/`, rebuild `mariadbd` and restart the server:

```bash
cd /Users/ryanzhou/Desktop/server

cmake --build build --target mariadbd --parallel 4
kill $(lsof -t /tmp/mariadb-parquet.sock)

build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

You do not need to rerun `mariadb-install-db` unless:

- you deleted `build/parquet-data`
- bootstrap previously failed and left grants half-created
- you intentionally want a fresh local data directory

## 11. Full clean rebuild

Removing `build/` is a full local reset. It deletes:

- the compiled binaries
- `build/mariadb-parquet.cnf`
- `build/parquet-data`
- `build/parquet-tmp`
- the local error log and pid file

Use this when you want to wipe all local build outputs and start over:

```bash
kill $(lsof -t /tmp/mariadb-parquet.sock)

cp /Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf /tmp/mariadb-parquet.cnf.backup
rm -rf /Users/ryanzhou/Desktop/server/build

cd /Users/ryanzhou/Desktop/server

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target minbuild --parallel 4
cmake --build build --target mariadbd --parallel 4

cp /tmp/mariadb-parquet.cnf.backup /Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
mkdir -p /Users/ryanzhou/Desktop/server/build/parquet-tmp

cd /Users/ryanzhou/Desktop/server/lakekeeper/examples/minimal
docker compose up -d
curl http://localhost:8181/health

cd /Users/ryanzhou/Desktop/server

build/scripts/mariadb-install-db \
  --force \
  --auth-root-authentication-method=socket \
  --auth-root-socket-user="$(whoami)" \
  --srcdir=/Users/ryanzhou/Desktop/server \
  --builddir=/Users/ryanzhou/Desktop/server/build \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf

build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 12. Minimal local `mariadb-parquet.cnf`

If `build/mariadb-parquet.cnf` is missing, this is the minimal working local shape for this checkout:

```ini
[mariadbd]
basedir=/Users/ryanzhou/Desktop/server/build
datadir=/Users/ryanzhou/Desktop/server/build/parquet-data
socket=/tmp/mariadb-parquet.sock
port=3307
pid-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.pid
log-error=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.err
tmpdir=/Users/ryanzhou/Desktop/server/build/parquet-tmp
plugin-dir=/Users/ryanzhou/Desktop/server/build/mysql-test/var/plugins
plugin-maturity=unknown
bind-address=127.0.0.1
character-set-server=utf8mb4
collation-server=utf8mb4_uca1400_ai_ci
secure-file-priv=

[client]
socket=/tmp/mariadb-parquet.sock
port=3307
```

## 13. Notes and limits

- `UPDATE` and `DELETE` are not implemented for Parquet tables.
- The MariaDB schema can be `test`, but the LakeKeeper namespace comes from `.env`.
- `DROP TABLE` behavior depends on LakeKeeper being reachable.
- If reads return nothing or table creation fails, inspect `/Users/ryanzhou/Desktop/server/build/mariadb-parquet.err`.

## 14. Common issues

### `scripts/mariadb-install-db: no such file or directory`

You are probably running the source-tree path or the wrong working directory. Use:

```bash
/Users/ryanzhou/Desktop/server/build/scripts/mariadb-install-db
```

or run `build/scripts/mariadb-install-db` from the repo root.

### `Can't change dir to '/Users/ryanzhou/Desktop/server/build/parquet-data/'`

The datadir does not exist yet. Run the bootstrap command in section 5 first.

### `LakeKeeper delete table error: Couldn't connect to server`

LakeKeeper is not up, the base URL is wrong, or `.env` is stale.

Check:

- `curl http://localhost:8181/health`
- `PARQUET_LAKEKEEPER_BASE_URL=http://localhost:8181/catalog/v1/`
- the warehouse UUID in `.env`
- the namespace in `.env`

If bootstrap already failed once, remove `build/parquet-data` and rerun it after LakeKeeper is healthy.

### `ERROR 1045 (28000): Access denied for user ...`

Most common causes:

- bootstrap failed partway through and left grant tables incomplete
- the datadir was initialized with a different auth mode than the one you are trying to use
- you are connecting without `--protocol=SOCKET` after a socket-auth bootstrap

For the local socket-auth flow in section 5, the safe recovery is:

1. Start LakeKeeper.
2. Stop MariaDB on `/tmp/mariadb-parquet.sock`.
3. Remove `build/parquet-data`.
4. Rerun `mariadb-install-db`.
5. Connect with `build/client/mariadb --defaults-file=... --protocol=SOCKET`.

### Query returns correct rows but Parquet debug lines do not appear

Make sure the running executable is:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```
