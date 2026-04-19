# Parquet Engine Setup Instructions

These instructions are written to be portable across local checkouts.

Assumptions:

- you are starting from the MariaDB server repo root
- your build directory is named `build`
- you have a MariaDB defaults file for this test setup
- you want to use LakeKeeper plus S3-compatible storage

If your build directory or config file uses a different name, substitute your local values.

## 1. Build the server

From the repo root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target mariadbd --parallel 4
```

Important:

- rebuild `mariadbd`, not only the `parquet` target, after changing Parquet engine code
- when running the server, use the binary from `build/sql/mariadbd`, not `sql/mariadbd` from the source tree

## 2. Configure runtime settings in `.env`

The Parquet engine now reads its LakeKeeper and S3 settings from a `.env` file in the repo root.

Create or update:

```text
.env
```

Supported keys:

- `PARQUET_LAKEKEEPER_BASE_URL`
- `PARQUET_LAKEKEEPER_WAREHOUSE_ID`
- `PARQUET_LAKEKEEPER_NAMESPACE`
- `PARQUET_S3_BUCKET`
- `PARQUET_S3_DATA_PREFIX`
- `PARQUET_S3_REGION`
- `PARQUET_S3_ACCESS_KEY_ID`
- `PARQUET_S3_SECRET_ACCESS_KEY`

Example:

```dotenv
PARQUET_LAKEKEEPER_BASE_URL=http://localhost:8181
PARQUET_LAKEKEEPER_WAREHOUSE_ID=REPLACE_WITH_WAREHOUSE_UUID
PARQUET_LAKEKEEPER_NAMESPACE=default
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

If you do not already have LakeKeeper running locally:

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

Create the warehouse once, using your real bucket and credentials:

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

## 5. Create the namespace

If your `.env` uses `PARQUET_LAKEKEEPER_NAMESPACE=default`, create that namespace once:

```bash
curl -X POST \
  http://localhost:8181/catalog/v1/REPLACE_WITH_WAREHOUSE_UUID/namespaces \
  -H "Content-Type: application/json" \
  -d '{"namespace":["default"]}'
```

If you use a different namespace in `.env`, replace `default` in that request.

## 6. Initialize the MariaDB data directory

From the build directory:

```bash
cd build
scripts/mariadb-install-db \
  --srcdir=.. \
  --defaults-file=/absolute/path/to/your/mariadb-parquet.cnf
```

Use your own MariaDB defaults file path here.

## 7. Start the server

From the build directory:

```bash
cd build
./sql/mariadbd --defaults-file=/absolute/path/to/your/mariadb-parquet.cnf
```

Important:

- use `./sql/mariadbd` from the build directory
- do not run `sql/mariadbd` from the source tree by mistake

If you want to verify the running executable:

```bash
lsof -p $(pgrep -n mariadbd) | awk '$4=="txt" {print $9}'
```

Expected shape:

```text
.../build/sql/mariadbd
```

## 8. Rebuild and restart after Parquet code changes

If you change files under `storage/parquet/`, rebuild `mariadbd`:

```bash
cmake --build build --target mariadbd --parallel 4
```

Then restart the server so the new code is loaded.

## 9. Connect to MariaDB

From the repo root:

```bash
build/client/mariadb --defaults-file=/absolute/path/to/your/mariadb-parquet.cnf
```

If your defaults file does not define the database, socket, or port, pass those explicitly.

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

Expected result:

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

Use one Parquet table and one non-Parquet table:

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

## 12. Check select-handler logs

After running the cross-engine queries, inspect the server log from your build:

```bash
grep "Parquet:" build/mariadb-parquet.err
```

For mixed-engine pushdown, you should see lines like:

- `Parquet: selected cross-engine pushdown handler`
- `Parquet: cross-engine pushdown init parquet_tables=[...] external_tables=[...]`
- `Parquet: cross-engine registry add key='...'`
- `Parquet: replacement scan lookup schema='...' table='...'`
- `Parquet: _mdb_scan bind table='...' columns=...`
- `Parquet: _mdb_scan start table='...' projected_columns=...`
- `Parquet: _mdb_scan complete table='...' rows=...`

If the query result is correct but these lines do not appear, you are probably not running the rebuilt `build/sql/mariadbd` binary.

## 13. Notes and current limits

- `UPDATE` and `DELETE` are not implemented
- the MariaDB schema and the LakeKeeper namespace are separate concepts
- the LakeKeeper namespace comes from `.env`
- after editing `.env`, restart the server
- if reads return nothing, check the build’s error log

## 14. Common issues

### `Plugin 'parquet' already installed`

That is fine. Skip `INSTALL PLUGIN`.

### `No database selected`

Run:

```sql
CREATE DATABASE IF NOT EXISTS test;
USE test;
```

### Query results look stale after a code change

Rebuild `mariadbd` and restart the server from the build directory.

### The server starts, but new logging or fixes do not appear

You are probably running the wrong binary. Start the server from:

```bash
cd build
./sql/mariadbd --defaults-file=/absolute/path/to/your/mariadb-parquet.cnf
```
