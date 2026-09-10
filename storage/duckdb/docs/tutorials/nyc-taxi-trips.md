# MariaDB DuckDB Tutorial: NYC Taxi Trips

## Overview

This tutorial follows the [ClickHouse taxi tutorial][clickhouse-tutorial] and its
[pg_clickhouse port][pg-clickhouse-tutorial], but runs everything on MariaDB with the DuckDB
storage engine. The trip data goes into an `ENGINE=DuckDB` table, the taxi zone lookup goes into an
`ENGINE=InnoDB` table, and a single MariaDB `SELECT` joins the two.

The data is published by the [NYC Taxi and Limousine Commission][tlc-data] and is subject to the
[NYC Open Data terms of use][nyc-open-data]. The TLC notes that the trip records come from
authorised technology providers rather than from the TLC itself, and makes no accuracy guarantees.

## Connect to MariaDB

The DuckDB plugin must be loaded and `run_in_duckdb()` must be enabled:

```ini
[mysqld]
plugin-maturity=gamma
plugin-load-add=ha_duckdb.so
duckdb-allow-run-in-duckdb=ON
```

See the [DuckDB storage engine README](../../README.md#building) for build and installation
instructions.

> **Note:** `run_in_duckdb()` is OFF by default and requires the `SUPER` privilege. It executes
> arbitrary DuckDB SQL in-process as the server OS user, and DuckDB applies no access control of its
> own. Enable it only on a host you control, and read
> [`security-model.md`](../security-model.md) before using it in a shared or multi-tenant
> deployment.

Start the client with UTF-8 and local file loading enabled:

```sh
mariadb --default-character-set=utf8mb4 --local-infile=1
```

Check that the engine, `run_in_duckdb()`, and local file loading are available:

```sql
SHOW ENGINES;
SELECT @@duckdb_allow_run_in_duckdb;
SHOW VARIABLES LIKE 'local_infile';
```

`SHOW ENGINES` should list `DUCKDB` with `Support` set to `YES`,
`duckdb_allow_run_in_duckdb` should be `1`, and `local_infile` should be `ON`.

## Download the Data Set

Run these commands on the MariaDB server host. DuckDB reads the Parquet file inside the server
process, so the account running `mariadbd` must be able to read it. The lookup CSV is loaded from
the client host via `LOAD DATA LOCAL INFILE`.

Take one month of Yellow Taxi trips plus the taxi zone lookup:

```sh
curl --fail --location -o /tmp/yellow_tripdata_2024-01.parquet \
  https://d37ci6vzurychx.cloudfront.net/trip-data/yellow_tripdata_2024-01.parquet

curl --fail --location -o /tmp/taxi_zone_lookup.csv \
  https://d37ci6vzurychx.cloudfront.net/misc/taxi_zone_lookup.csv
```

The Parquet file is about 48 MiB and holds 2,964,624 trips. The lookup CSV holds 265 zones.

## Create a Table

```sql
CREATE DATABASE IF NOT EXISTS taxi;
USE taxi;

CREATE TABLE trips (
  trip_id           BIGINT NOT NULL,
  vendor_id         INT,
  pickup_datetime   DATETIME,
  dropoff_datetime  DATETIME,
  passenger_count   INT,
  trip_distance     DOUBLE,
  pu_location_id    INT,
  do_location_id    INT,
  payment_type      INT,
  fare_amount       DECIMAL(10,2),
  tip_amount        DECIMAL(10,2),
  total_amount      DECIMAL(10,2),
  PRIMARY KEY (trip_id)
) ENGINE=DuckDB DEFAULT CHARSET=utf8mb4;
```

With the default `duckdb_require_primary_key=ON`, the table needs a primary key. The TLC files have
no trip identifier, so the load below generates `trip_id`. DuckDB table columns accept `utf8`,
`utf8mb3`, `utf8mb4`, and `ascii`; this example uses `utf8mb4`.

## Add the Data Set

`run_in_duckdb()` sends the SQL string to DuckDB's vectorized engine for execution and returns DuckDB's own textual rendering of the result as a single string value. DuckDB reads the Parquet file directly, so the load is a single statement:

```sql
SELECT run_in_duckdb('INSERT INTO taxi.trips
  (trip_id, vendor_id, pickup_datetime, dropoff_datetime, passenger_count,
   trip_distance, pu_location_id, do_location_id, payment_type,
   fare_amount, tip_amount, total_amount)
SELECT
  ROW_NUMBER() OVER () AS trip_id,
  "VendorID", tpep_pickup_datetime, tpep_dropoff_datetime, passenger_count,
  trip_distance, "PULocationID", "DOLocationID", payment_type,
  fare_amount, tip_amount, total_amount
FROM read_parquet(''/tmp/yellow_tripdata_2024-01.parquet'')');
```

The TLC column names are mixed case, so they appear as double-quoted DuckDB identifiers. Only the
twelve selected columns are read from the file.

`run_in_duckdb()` is the bulk-load path, not the only write path: ordinary `INSERT`, `UPDATE`, and
`DELETE` work against an `ENGINE=DuckDB` table too. With the default `duckdb_dml_in_batch=ON` those
writes are buffered and applied to DuckDB at commit, so a write is not visible to later reads in the
same transaction. This tutorial only reads, so it loads once and queries from there.

Make sure we can query it:

```sql
SELECT COUNT(*) AS total_trips FROM trips;
```

```text
total_trips
2964624
```

The pickup timestamps are worth a look before trusting any date filter:

```sql
SELECT MIN(pickup_datetime) AS earliest, MAX(pickup_datetime) AS latest FROM trips;
```

```text
earliest             latest
2002-12-31 22:59:39  2024-02-01 00:01:15
```

The January 2024 file holds 18 rows with timestamps outside the month, too few to move any of the
averages that follow. The queries that group or rank by time still filter on the month explicitly,
so their buckets cover exactly the period they claim to.

## Analyze the Data

Calculate the average tip amount:

```sql
SELECT ROUND(AVG(tip_amount), 2) AS avg_tip FROM trips;
```

```text
avg_tip
3.34
```

Calculate the average cost based on the number of passengers:

```sql
SELECT passenger_count,
       COUNT(*) AS trips,
       ROUND(AVG(total_amount), 2) AS avg_total
FROM trips
WHERE passenger_count IS NOT NULL
GROUP BY passenger_count
ORDER BY passenger_count;
```

```text
passenger_count  trips    avg_total
0                31465    25.33
1                2188739  26.21
2                405103   29.52
3                91262    29.14
4                51974    30.88
5                33506    26.27
6                22353    25.80
7                8        57.74
8                51       95.67
9                1        18.45
```

Show the busiest pickup hours:

```sql
SELECT HOUR(pickup_datetime) AS pickup_hour,
       COUNT(*) AS trips,
       ROUND(AVG(trip_distance), 2) AS avg_miles
FROM trips
WHERE pickup_datetime >= '2024-01-01' AND pickup_datetime < '2024-02-01'
GROUP BY HOUR(pickup_datetime)
ORDER BY trips DESC
LIMIT 5;
```

```text
pickup_hour  trips   avg_miles
18           212788  2.81
17           206257  3.01
16           190201  3.35
15           189359  3.88
19           184032  3.11
```

Group trips into ten-minute buckets by duration:

```sql
SELECT FLOOR((UNIX_TIMESTAMP(dropoff_datetime) -
              UNIX_TIMESTAMP(pickup_datetime)) / 600) * 10 AS trip_minutes,
       COUNT(*) AS trips,
       ROUND(AVG(fare_amount), 2) AS avg_fare,
       ROUND(AVG(tip_amount), 2) AS avg_tip
FROM trips
WHERE dropoff_datetime > pickup_datetime
GROUP BY FLOOR((UNIX_TIMESTAMP(dropoff_datetime) -
                UNIX_TIMESTAMP(pickup_datetime)) / 600) * 10
ORDER BY trip_minutes
LIMIT 6;
```

```text
trip_minutes  trips    avg_fare  avg_tip
0             1229720  8.77      1.98
10            1080498  15.77     3.00
20            382804   29.13     5.06
30            142500   47.72     7.84
40            64520    59.82     9.35
50            33057    65.80     9.64
```

`TIMESTAMPDIFF()` is not pushed down — its `unit` keyword is not bindable, so the call reaches
DuckDB as an unknown function and fails on `ENGINE=DuckDB` tables. This query uses
`UNIX_TIMESTAMP()` arithmetic instead. See
[`mariadb-duckdb-incompatibilities.md`](../mariadb-duckdb-incompatibilities.md) for the full list of
pushdown-incompatible functions and syntax.

## Add the Zone Lookup Table

The trips carry zone IDs rather than names. The lookup table maps each ID to a borough and zone
name; `132` is JFK Airport and `138` is LaGuardia Airport. Keep it in InnoDB:

```sql
CREATE TABLE taxi_zones (
  location_id   INT PRIMARY KEY,
  borough       VARCHAR(30),
  zone          VARCHAR(60),
  service_zone  VARCHAR(20)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

LOAD DATA LOCAL INFILE '/tmp/taxi_zone_lookup.csv'
INTO TABLE taxi_zones
FIELDS TERMINATED BY ',' OPTIONALLY ENCLOSED BY '"'
LINES TERMINATED BY '\r\n'
IGNORE 1 LINES
(location_id, borough, zone, @service_zone)
SET service_zone = NULLIF(@service_zone, '');
```

The lookup CSV uses CRLF line endings, hence `LINES TERMINATED BY '\r\n'`.

Confirm the row count and the engines:

```sql
SELECT COUNT(*) AS zones FROM taxi_zones;
SELECT TABLE_NAME, ENGINE
FROM information_schema.TABLES
WHERE TABLE_SCHEMA = 'taxi'
ORDER BY TABLE_NAME;
```

```text
zones
265

TABLE_NAME  ENGINE
taxi_zones  InnoDB
trips       DUCKDB
```

## Perform a Cross-Engine Join

The zone lookup is small and changes rarely, so it stays in InnoDB. The trip data is large and
analytical, so it lives in DuckDB. A single MariaDB `SELECT` joins them — no `run_in_duckdb()`, no
data copying, no ETL. The engine pushes the entire query down to DuckDB and streams the InnoDB rows
in on demand.

Count the trips ending at JFK or LaGuardia, grouped by the borough where the passenger got in:

```sql
SELECT z.borough,
       COUNT(*) AS airport_trips,
       ROUND(AVG(t.total_amount), 2) AS avg_total
FROM trips t
JOIN taxi_zones z ON t.pu_location_id = z.location_id
WHERE t.do_location_id IN (132, 138)
GROUP BY z.borough
ORDER BY airport_trips DESC;
```

```text
borough        airport_trips  avg_total
Manhattan      48077          78.09
Queens         13789          43.41
Brooklyn       738            60.82
Unknown        109            64.48
Bronx          27             63.63
N/A            21             94.95
EWR            1              135.18
Staten Island  1              11.50
```

`trips` stays in DuckDB and `taxi_zones` stays in InnoDB. There is no `run_in_duckdb()` call and no
copy of the data: this is a plain MariaDB `SELECT` across two storage engines.

To see what MariaDB sent to DuckDB, turn on query logging and run the join again:

```sql
SET GLOBAL duckdb_log_options = 'DUCKDB_QUERY';
```

The server error log then shows the pushed-down statement and the external table:

```text
[Note] DuckDB: cross-engine pushdown with 1 external table(s)
[Note] DuckDB query: SELECT z.borough, COUNT(*) AS airport_trips, ROUND(AVG(t.total_amount), 2) ...
```

DuckDB received the whole query, joins and aggregates included, and MariaDB streamed the InnoDB
rows into it. Turn logging off again:

```sql
SET GLOBAL duckdb_log_options = '';
```

One more join, this time ranking pickup zones for the month:

```sql
SELECT z.zone,
       COUNT(*) AS pickups,
       ROUND(AVG(t.tip_amount), 2) AS avg_tip
FROM trips t
JOIN taxi_zones z ON t.pu_location_id = z.location_id
WHERE t.pickup_datetime >= '2024-01-01' AND t.pickup_datetime < '2024-02-01'
GROUP BY z.zone
ORDER BY pickups DESC
LIMIT 10;
```

```text
zone                          pickups  avg_tip
JFK Airport                   145240   8.86
Midtown Center                143469   3.08
Upper East Side South         142707   2.59
Upper East Side North         136464   2.64
Midtown East                  106717   3.02
Times Sq/Theatre District     106324   3.30
Penn Station/Madison Sq West  104522   3.09
Lincoln Square East           104080   2.79
LaGuardia Airport             89530    8.67
Upper West Side South         88474    2.79
```

Airport pickups tip roughly three times as much as midtown ones.

## Clean Up

```sql
DROP DATABASE taxi;
```

The [OWID CO₂ tutorial](owid-co2-emissions.md) loads CSV instead of Parquet and computes
year-over-year change with a `LAG()` window function.

[clickhouse-tutorial]: https://clickhouse.com/docs/tutorial
[nyc-open-data]: https://opendata.cityofnewyork.us/overview/
[pg-clickhouse-tutorial]: https://github.com/ClickHouse/pg_clickhouse/blob/b2bacc51e1205f840046fe9f48f806baa6ae169e/doc/tutorial.md
[tlc-data]: https://www.nyc.gov/site/tlc/about/tlc-trip-record-data.page
