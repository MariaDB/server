# Local Setup Instructions (`/Users/ryanzhou/Desktop/server`)

These instructions match the current working flow in this checkout.

- source tree: `/Users/ryanzhou/Desktop/server`
- build tree: `/Users/ryanzhou/Desktop/server/build`
- runtime env file: `/Users/ryanzhou/Desktop/server/.env`
- MariaDB config: `/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf`
- socket: `/tmp/mariadb-parquet.sock`
- LakeKeeper warehouse UUID: `9a3b8dae-3bab-11f1-aa80-237d1c73ff26`

This Parquet engine currently depends on:

- LakeKeeper at `http://localhost:8181`
- an S3 bucket named `mariadb-parquet-demo`
- valid AWS credentials in `/Users/ryanzhou/Desktop/server/.env`

## 1. Build the server

Build `mariadbd`, not only the Parquet static library:

```bash
cd /Users/ryanzhou/Desktop/server
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /Users/ryanzhou/Desktop/server/build --target mariadbd --parallel 4
```

## 2. Update `.env`

Edit:

`/Users/ryanzhou/Desktop/server/.env`

Current keys:

- `PARQUET_LAKEKEEPER_BASE_URL`
- `PARQUET_LAKEKEEPER_WAREHOUSE_ID`
- `PARQUET_LAKEKEEPER_NAMESPACE`
- `PARQUET_S3_BUCKET`
- `PARQUET_S3_DATA_PREFIX`
- `PARQUET_S3_REGION`
- `PARQUET_S3_ACCESS_KEY_ID`
- `PARQUET_S3_SECRET_ACCESS_KEY`

The warehouse UUID should currently be:

```text
9a3b8dae-3bab-11f1-aa80-237d1c73ff26
```

If you want to use a different env file, set:

```bash
export PARQUET_ENV_FILE=/absolute/path/to/your/.env
```

After editing `.env`, restart the server so `mariadbd` reloads the values:

```bash
kill $(pgrep -n mariadbd)

/Users/ryanzhou/Desktop/server/build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 3. Start LakeKeeper

If LakeKeeper is not already running:

```bash
git clone https://github.com/lakekeeper/lakekeeper
cd lakekeeper/examples/minimal
docker compose up -d
```

Verify it:

```bash
curl http://localhost:8181/health
```

## 4. Create the warehouse

If the warehouse does not already exist:

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

## 5. Create the `default` namespace

The default namespace in `.env` is `default`.

Create it once:

```bash
curl -X POST \
  http://localhost:8181/catalog/v1/9a3b8dae-3bab-11f1-aa80-237d1c73ff26/namespaces \
  -H "Content-Type: application/json" \
  -d '{"namespace":["default"]}'
```

## 6. Initialize the MariaDB data directory

```bash
cd /Users/ryanzhou/Desktop/server/build
scripts/mariadb-install-db \
  --srcdir=/Users/ryanzhou/Desktop/server \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 7. Start MariaDB with the build binary

Use the absolute build path. Do not use `/Users/ryanzhou/Desktop/server/sql/mariadbd`.

```bash
mkdir -p build/parquet-tmp

/Users/ryanzhou/Desktop/server/build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

If you want to verify the running executable:

```bash
lsof -p $(pgrep -n mariadbd) | awk '$4=="txt" {print $9}'
```

Expected:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```

## 8. Rebuild and restart after Parquet handler/select-handler changes

If you change code under:

- `/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc`
- `/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc`
- `/Users/ryanzhou/Desktop/server/storage/parquet/parquet_cross_engine_scan.cc`

rebuild `mariadbd`, not just the `parquet` target:

```bash
cd /Users/ryanzhou/Desktop/server
cmake --build /Users/ryanzhou/Desktop/server/build --target mariadbd --parallel 4
```

Then restart the running server with the build binary:

```bash
kill $(lsof -t /tmp/mariadb-parquet.sock)

/Users/ryanzhou/Desktop/server/build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 9. Connect to MariaDB

In another terminal:

```bash
cd /Users/ryanzhou/Desktop/server
build/client/mariadb --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 10. Create and query a Parquet table

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

## 11. Test cross-engine join pushdown

To test the SELECT handler’s mixed-engine path, use one Parquet table and one non-Parquet table:

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

If the query returns the right result but none of those lines appear, you are probably not running the rebuilt `mariadbd` binary from:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```

## 12. Notes and limits

- `UPDATE` and `DELETE` are not implemented
- the MariaDB schema can be `test`, but the LakeKeeper namespace comes from `.env`
- `DROP TABLE` only works cleanly after the newer delete hook is running
- if reads return nothing, inspect `/Users/ryanzhou/Desktop/server/build/mariadb-parquet.err`

## 13. Common issues

### `Plugin 'parquet' already installed`

That is fine. Skip `INSTALL PLUGIN`.

### `No database selected`

Run:

```sql
CREATE DATABASE IF NOT EXISTS test;
USE test;
```

### `Can't create table ... Internal (unspecified) error in handler`

Most likely causes:

- LakeKeeper warehouse UUID is wrong
- the namespace in `.env` does not exist in LakeKeeper
- S3 credentials are invalid

### Reads work only after restarting with the right binary

Make sure the running executable is:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```
